/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/**
 * implementation for C++ XML generation class
 *
 * The software provided here is released by the Naval Postgraduate
 * School, an agency of the U.S. Department of Navy.  The software
 * bears no warranty, either expressed or implied. NPS does not assume
 * legal liability nor responsibility for a User's use of the software
 * or the results of such use.
 *
 * Please note that within the United States, copyright protection,
 * under Section 105 of the United States Code, Title 17, is not
 * available for any work of the United States Government and/or for
 * any works created by United States Government employees. User
 * acknowledges that this software contains work which was created by
 * NPS government employees and is therefore in the public domain and
 * not subject to copyright.
 */


#include "config.h"
#include "dfxml_writer.h"

#ifdef HAVE_WINSOCK2_H
#include <winsock2.h>
#endif

#include <sys/param.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef _MSC_VER
# include <io.h>
#else
# include <unistd.h>
#endif

#ifdef HAVE_SQLITE3_H
#include <sqlite3.h>
#endif

#ifdef HAVE_BOOST_VERSION_HPP
#include <boost/version.hpp>
#endif

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stack>
#include <streambuf>


#include "dfxml_writer.h"
#include "cpuid.h"

static const char *xml_header = "<?xml version='1.0' encoding='UTF-8'?>\n";

// Implementation of mkstemp for windows found on pan-devel mailing
// list archive
// @http://www.mail-archive.com/pan-devel@nongnu.org/msg00294.html
#ifndef _S_IREAD
#define _S_IREAD 256
#endif

#ifndef _S_IWRITE
#define _S_IWRITE 128
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef _O_SHORT_LIVED
#define _O_SHORT_LIVED 0
#endif

#ifndef HAVE_MKSTEMP
int mkstemp(char *tmpl)
{
   int ret=-1;
   mktemp(tmpl);
   ret=open(tmpl,O_RDWR|O_BINARY|O_CREAT|O_EXCL|_O_SHORT_LIVED, _S_IREAD|_S_IWRITE);
   return ret;
}
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef _O_SHORT_LIVED
#define _O_SHORT_LIVED 0
#endif

//std::string dfxml_writer::xml_PRId32("%" PRId32); // gets around compiler bug
//std::string dfxml_writer::xml_PRIu32("%" PRIu32); // gets around compiler bug
//std::string dfxml_writer::xml_PRId64("%" PRId64); // gets around compiler bug
//std::string dfxml_writer::xml_PRIu64("%" PRIu64); // gets around compiler bug

static const char *cstr(const std::string &str){
    return str.c_str();
}

// XML escapes
static std::string xml_lt("&lt;");
static std::string xml_gt("&gt;");
static std::string xml_am("&amp;");
static std::string xml_ap("&apos;");
static std::string xml_qu("&quot;");

// % encodings
static std::string encoding_null("%00");
static std::string encoding_r("%0D");
static std::string encoding_n("%0A");
static std::string encoding_t("%09");

std::string dfxml_writer::make_command_line(int argc,char * const *argv)
{
    std::string command_line;
    for(int i=0;i<argc;i++){
        // append space separator between arguments
        if(i>0) command_line.push_back(' ');
        if (strchr(argv[i],' ') != NULL) {
            // the argument has a space, so quote the argument
            command_line.append("\"");
            command_line.append(argv[i]);
            command_line.append("\"");
        } else {
            // the argument has no space, so append as is
            command_line.append(argv[i]);
        }
    }
    return command_line;
}



std::string dfxml_writer::xmlescape(const std::string &xml)
{
    std::string ret;
    for(auto const &ch: xml){
        switch(ch){
        // XML escapes
        case '>':  ret += xml_gt; break;
        case '<':  ret += xml_lt; break;
        case '&':  ret += xml_am; break;
        case '\'': ret += xml_ap; break;
        case '"':  ret += xml_qu; break;

        // % encodings
        case '\000':  ret += encoding_null; break;      // retain encoded nulls
        case '\r':  ret += encoding_r; break;
        case '\n':  ret += encoding_n; break;
        case '\t':  ret += encoding_t; break;
        default:
            ret += ch;
        }
    }
    return ret;
}

/**
 * Strip an XML std::string as necessary for a tag name.
 */

std::string dfxml_writer::xmlstrip(const std::string &xml)
{
    std::string ret;
    for( const auto &ch : xml){
        if(isprint(ch) && !strchr("<>\r\n&'\"",ch)){
            ret += isspace(ch) ? '_' : tolower(ch);
        }
    }
    return ret;
}

/**
 * xmlmap:
 * Turns a map into a blob of XML.
 */

std::string dfxml_writer::xmlmap(const dfxml_writer::strstrmap_t &m,const std::string &outer,const std::string &attrs)
{
    std::stringstream ss;
    ss << "<" << outer;
    if(attrs.size()>0) ss << " " << attrs;
    ss << ">";
    for(std::map<std::string,std::string>::const_iterator it=m.begin();it!=m.end();it++){
        ss << "<" << (*it).first  << ">" << xmlescape((*it).second) << "</" << (*it).first << ">";
    }
    ss << "</" << outer << ">";
    return ss.str();
}


/* This goes to stdout */
dfxml_writer::dfxml_writer():M(),outf(),out(&std::cout),tags(),tag_stack(),tempfilename(),tempfile_template("/tmp/xml_XXXXXXXX"),
           t0(),t_last_timestamp(),make_dtd(false),outfilename(),oneline()
{
    gettimeofday(&t0,0);
    gettimeofday(&t_last_timestamp,0);
    *out << xml_header;
}

/* This should be rewritten so that the temp file is done on close, not on open */
dfxml_writer::dfxml_writer(const std::string &outfilename_,bool makeDTD):
    M(),outf(outfilename_.c_str(),std::ios_base::out),
    out(),tags(),tag_stack(),tempfilename(),tempfile_template(outfilename_+"_tmp_XXXXXXXX"),
    t0(),t_last_timestamp(),make_dtd(false),outfilename(outfilename_),oneline()
{
    gettimeofday(&t0,0);
    gettimeofday(&t_last_timestamp,0);
    if(!outf.is_open()){
        perror(outfilename_.c_str());
        exit(1);
    }
    out = &outf;                                                // use this one instead
    *out << xml_header;
}


void dfxml_writer::add_DFXML_creator(const std::string &program,const std::string &version,
                                     const std::string &commit,int argc, char * const *argv){
    const std::string command_line = make_command_line(argc,argv);

    push("creator","version='1.0'");
    xmlout("program",program);
    xmlout("version",version);
    if(commit.size()>0) xmlout("commit",commit);
    add_DFXML_build_environment();
    add_DFXML_execution_environment(command_line);
    pop();                  // creator
}


void dfxml_writer::set_tempfile_template(const std::string &temp)
{
    tempfile_template = temp;
}




void dfxml_writer::close()
{
    const std::lock_guard<std::mutex> lock(M);
    outf.close();
    if(make_dtd){
        /* If we are making the DTD, then we should close the file,
         * scan the output file for the tags, write to a temp file, and then
         * close the temp file and have it overwrite the outfile.
         */

        std::ifstream in(cstr(tempfilename));
        if(!in.is_open()){
            std::cerr << tempfilename << strerror(errno) << ":Cannot re-open for input\n";
            exit(1);
        }
        outf.open(cstr(outfilename),std::ios_base::out);
        if(!outf.is_open()){
            std::cerr << outfilename << " " << strerror(errno)
                 << ": Cannot open for output; will not delete " << tempfilename << "\n";
            exit(1);
        }
        // copy over first line --- the XML header
        std::string line;
        getline(in,line);
        outf << line;

        write_dtd();                    // write the DTD
        while(!in.eof()){
            getline(in,line);
            outf << line << std::endl;
        }
        in.close();
        unlink(cstr(tempfilename));
        outf.close();
    }
}

void dfxml_writer::write_dtd()
{
    *out << "<!DOCTYPE fiwalk\n";
    *out << "[\n";
    for (auto const &it:tags) {
        *out << "<!ELEMENT " << it << "ANY >\n";
    }
    *out << "<!ATTLIST volume startsector CDATA #IMPLIED>\n";
    *out << "<!ATTLIST run start CDATA #IMPLIED>\n";
    *out << "<!ATTLIST run len CDATA #IMPLIED>\n";
    *out << "]>\n";
}

/**
 * make sure that a tag is valid and, if so, add it to the list of tags we use
 */
void dfxml_writer::verify_tag(std::string tag)
{
    if(tag[0]=='/') tag = tag.substr(1);
    if(tag.find(" ") != std::string::npos){
        std::cerr << "tag '" << tag << "' contains space. Cannot continue.\n";
        exit(1);
    }
    tags.insert(tag);
}

void dfxml_writer::puts(const std::string &v)
{
    *out << v;
}

void dfxml_writer::spaces()
{
    for(unsigned int i=0;i<tag_stack.size() && !oneline;i++){
        *out << "  ";
    }
}

void dfxml_writer::tagout(const std::string &tag,const std::string &attribute)
{
    verify_tag(tag);
    *out << "<" << tag;
    if(attribute.size()>0) *out << " " << attribute;
    *out << ">";
}

#if (!defined(HAVE_VASPRINTF)) || defined(_WIN32)
#ifndef _WIN32
#define ms_printf __print
#define __MINGW_ATTRIB_NONNULL(x)
#endif
extern "C" {
    /**
     * We do not have vasprintf.
     * We have determined that vsnprintf() does not perform properly on windows.
     * So we just allocate a huge buffer and then strdup() and hope!
     */
    int vasprintf(char **ret,const char *fmt,va_list ap)
        __attribute__((__format__(ms_printf, 2, 0)))
        __MINGW_ATTRIB_NONNULL(2) ;
    int vasprintf(char **ret,const char *fmt,va_list ap)
    {
        /* Figure out how long the result will be */
        char buf[65536];
        int size = vsnprintf(buf,sizeof(buf),fmt,ap);
        if(size<0) return size;
        /* Now allocate the memory */
        *ret = strcpy((char *) malloc(strlen(buf)+1), buf);
        return size;
    }
}
#endif


void dfxml_writer::printf(const char *fmt,...)
{
    va_list ap;
    va_start(ap, fmt);

    /** printf to stream **/
    char *ret = 0;
    if(vasprintf(&ret,fmt,ap) < 0){
        *out << "dfxml_writer::xmlprintf: " << strerror(errno);
        exit(EXIT_FAILURE);
    }
    *out << ret;
    free(ret);
    /** end printf to stream **/

    va_end(ap);
}

void dfxml_writer::push(const std::string &tag,const std::string &attribute)
{
    spaces();
    tag_stack.push(tag);
    tagout(tag,attribute);
    if(!oneline) *out << '\n';
}

void dfxml_writer::pop()
{
    assert(tag_stack.size()>0);
    std::string tag = tag_stack.top();
    tag_stack.pop();
    spaces();
    tagout("/"+tag,"");
    *out << '\n';
}

void dfxml_writer::set_oneline(bool v)
{
    if(v==true) spaces();
    if(v==false) *out << "\n";
    oneline = v;
}

void dfxml_writer::add_cpuid()
{
#ifdef HAVE_ASM_CPUID
#ifndef __WORDSIZE
#define __WORDSIZE 32
#endif
#define BFIX(val, base, end) ((val << (__WORDSIZE-end-1)) >> (__WORDSIZE-end+base-1))

    CPUID  cpuID(0);                     // get CPU vendor
    unsigned long eax = cpuID.EAX();
    unsigned long ebx = cpuID.EBX();
    unsigned long ecx = cpuID.ECX();

    push("cpuid");
    xmlout("identification", CPUID::vendor());
    xmlout("family",   (int64_t) BFIX(eax, 8, 11));
    xmlout("model",    (int64_t) BFIX(eax, 4, 7));
    xmlout("stepping", (int64_t) BFIX(eax, 0, 3));
    xmlout("efamily",  (int64_t) BFIX(eax, 20, 27));
    xmlout("emodel",   (int64_t) BFIX(eax, 16, 19));
    xmlout("brand",    (int64_t) BFIX(ebx, 0, 7));
    xmlout("clflush_size", (int64_t) BFIX(ebx, 8, 15) * 8);
    xmlout("nproc",    (int64_t) BFIX(ebx, 16, 23));
    xmlout("apicid",   (int64_t) BFIX(ebx, 24, 31));

    CPUID cpuID2(0x80000006);
    ecx = cpuID2.ECX();
    xmlout("L1_cache_size", (int64_t) BFIX(ecx, 16, 31) * 1024);
    pop();
#undef BFIX
#endif
}

void dfxml_writer::add_DFXML_execution_environment(const std::string &command_line)
{
    push("execution_environment");
#if defined(HAVE_ASM_CPUID)
    add_cpuid();
#endif

#ifdef HAVE_SYS_UTSNAME_H
    struct utsname name;
    if(uname(&name)==0){
        xmlout("os_sysname",name.sysname);
        xmlout("os_release",name.release);
        xmlout("os_version",name.version);
        xmlout("host",name.nodename);
        xmlout("arch",name.machine);
    }
#else
#ifdef UNAMES
    xmlout("os_sysname",UNAMES,"",false);
#endif
#ifdef HAVE_GETHOSTNAME
    {
        char hostname[1024];
        if(gethostname(hostname,sizeof(hostname))==0){
            xmlout("host",hostname);
        }
    }
#endif
#endif

    xmlout("command_line", command_line); // quote it!
#ifdef HAVE_GETUID
    xmlprintf("uid","","%d",getuid());
#ifdef HAVE_GETPWUID
    xmlout("username",getpwuid(getuid())->pw_name);
#endif
#endif

#define TM_FORMAT "%Y-%m-%dT%H:%M:%SZ"
    char buf[256];
    time_t t = time(0);
    strftime(buf,sizeof(buf),TM_FORMAT,gmtime(&t));
    xmlout("start_time",buf);
    pop();                      // <execution_environment>
}

#ifdef WIN32
#include "psapi.h"
#endif

void dfxml_writer::add_rusage()
{
#ifdef WIN32
    /* Note: must link -lpsapi for this */
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc,0,sizeof(pmc));
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc));
    push("PROCESS_MEMORY_COUNTERS");
    xmlout("cb",(int64_t)pmc.cb);
    xmlout("PageFaultCount",(int64_t)pmc.PageFaultCount);
    xmlout("WorkingSetSize",(int64_t)pmc.WorkingSetSize);
    xmlout("QuotaPeakPagedPoolUsage",(int64_t)pmc.QuotaPeakPagedPoolUsage);
    xmlout("QuotaPagedPoolUsage",(int64_t)pmc.QuotaPagedPoolUsage);
    xmlout("QuotaPeakNonPagedPoolUsage",(int64_t)pmc.QuotaPeakNonPagedPoolUsage);
    xmlout("PagefileUsage",(int64_t)pmc.PagefileUsage);
    xmlout("PeakPagefileUsage",(int64_t)pmc.PeakPagefileUsage);
    xmlout("PrivateUsage",(int64_t)pmc.PrivateUsage);
    pop();
#endif
#ifdef HAVE_GETRUSAGE
    struct rusage ru;
    memset(&ru,0,sizeof(ru));
    if(getrusage(RUSAGE_SELF,&ru)==0){
        push("rusage");
        xmlout("utime",ru.ru_utime);
        xmlout("stime",ru.ru_stime);
        xmloutl("maxrss",(long)ru.ru_maxrss);
        xmloutl("minflt",(long)ru.ru_minflt);
        xmloutl("majflt",(long)ru.ru_majflt);
        xmloutl("nswap",(long)ru.ru_nswap);
        xmloutl("inblock",(long)ru.ru_inblock);
        xmloutl("oublock",(long)ru.ru_oublock);

        struct timeval t1;
        gettimeofday(&t1,0);
        struct timeval t;

        t.tv_sec = t1.tv_sec - t0.tv_sec;
        if(t1.tv_usec > t0.tv_usec){
            t.tv_usec = t1.tv_usec - t0.tv_usec;
        } else {
            t.tv_sec--;
            t.tv_usec = (t1.tv_usec+1000000) - t0.tv_usec;
        }
        xmlout("clocktime",t);
        pop();
    }
#endif
}

void dfxml_writer::add_timestamp(const std::string &name)
{
    struct timeval t1;
    gettimeofday(&t1,0);
    struct timeval t;

    // timestamp delta against t_last_timestamp
    t.tv_sec = t1.tv_sec - t_last_timestamp.tv_sec;
    if(t1.tv_usec > t_last_timestamp.tv_usec){
        t.tv_usec = t1.tv_usec - t_last_timestamp.tv_usec;
    } else {
        t.tv_sec--;
        t.tv_usec = (t1.tv_usec+1000000) - t_last_timestamp.tv_usec;
    }
    char delta[16];
    snprintf(delta, 16, "%d.%06d", (int)t.tv_sec, (int)t.tv_usec);

    // reset t_last_timestamp for the next invocation
    gettimeofday(&t_last_timestamp,0);

    // timestamp total
    t.tv_sec = t1.tv_sec - t0.tv_sec;
    if(t1.tv_usec > t0.tv_usec){
        t.tv_usec = t1.tv_usec - t0.tv_usec;
    } else {
        t.tv_sec--;
        t.tv_usec = (t1.tv_usec+1000000) - t0.tv_usec;
    }
    char total[16];
    snprintf(total, 16, "%d.%06d", (int)t.tv_sec, (int)t.tv_usec);

    // prepare attributes
    std::stringstream ss;
    ss << "name='" << name
       << "' delta='" << delta
       << "' total='" << total
       << "'";

    // add named timestamp
    xmlout("timestamp", "",ss.str(), true);
}

/****************************************************************
 *** THESE ARE THE ONLY THREADSAFE ROUTINES
 ****************************************************************/
void dfxml_writer::comment(const std::string &comment_)
{
    const std::lock_guard<std::mutex> lock(M);
    *out << "<!-- " << comment_ << " -->\n";
    out->flush();
}


void dfxml_writer::xmlprintf(const std::string &tag,const std::string &attribute, const char *fmt,...)
{
    const std::lock_guard<std::mutex> lock(M);
    spaces();
    tagout(tag,attribute);
    va_list ap;
    va_start(ap, fmt);

    /** printf to stream **/
    char *ret = 0;
    if(vasprintf(&ret,fmt,ap) < 0){
        std::cerr << "dfxml_writer::xmlprintf: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    }
    *out << ret;
    free(ret);
    /** end printf to stream **/

    va_end(ap);
    tagout("/"+tag,"");
    *out << '\n';
    out->flush();
}

void dfxml_writer::xmlout(const std::string &tag,const std::string &value,const std::string &attribute,bool escape_value)
{
    const std::lock_guard<std::mutex> lock(M);
    spaces();
    if(value.size()==0){
        if(tag.size()) tagout(tag,attribute+"/");
    } else {
        if(tag.size()) tagout(tag,attribute);
        *out << (escape_value ? xmlescape(value) : value);
        if(tag.size()) tagout("/"+tag,"");
    }
    *out << "\n";
    out->flush();
}

#ifdef HAVE_LIBEWF_H
#include <libewf.h>
#endif

#ifdef HAVE_HASHDB
#include <hashdb.hpp>
#endif


/* These support Digital Forensics XML and require certain variables to be defined
 * TODO: Create a sytem to allow caller to register library callbacks or provide a list of libraries to add.
 */
void dfxml_writer::add_DFXML_build_environment()
{
    /* __DATE__ formats as: Apr 30 2011 */
    struct tm tm;
    memset(&tm,0,sizeof(tm));
    push("build_environment");
#ifdef __GNUC__
    // See http://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
    xmlprintf("compiler","","%d.%d.%d (%s)",__GNUC__, __GNUC_MINOR__,__GNUC_PATCHLEVEL__,__VERSION__);
#endif
#ifdef CPPFLAGS
    xmlout("CPPFLAGS",CPPFLAGS,"",true);
#endif
#ifdef CFLAGS
    xmlout("CFLAGS",CFLAGS,"",true);
#endif
#ifdef CXXFLAGS
    xmlout("CXXFLAGS",CXXFLAGS,"",true);
#endif
#ifdef LDFLAGS
    xmlout("LDFLAGS",LDFLAGS,"",true);
#endif
#ifdef LIBS
    xmlout("LIBS",LIBS,"",true);
#endif
#if defined(__DATE__) && defined(__TIME__) && defined(HAVE_STRPTIME)
    if(strptime(__DATE__,"%b %d %Y",&tm)){
        char buf[64];
        snprintf(buf,sizeof(buf),"%4d-%02d-%02dT%s",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,__TIME__);
        xmlout("compilation_date",buf);
    }
#endif
#ifdef BOOST_VERSION
    {
        char buf[64];
        snprintf(buf,sizeof(buf),"%d",BOOST_VERSION);
        xmlout("library", "", std::string("name=\"boost\" version=\"") + buf + "\"",false);
    }
#endif
#ifdef HAVE_LIBTSK3
    xmlout("library", "", std::string("name=\"tsk\" version=\"") + tsk_version_get_str() + "\"",false);
#endif
#ifdef HAVE_LIBEWF
    xmlout("library", "", std::string("name=\"libewf\" version=\"") + libewf_get_version() + "\"",false);
#endif
#ifdef HAVE_EXIV2
    xmlout("library", "", std::string("name=\"exiv2\" version=\"") + Exiv2::version() + "\"",false);
#endif
#ifdef HAVE_HASHDB
    xmlout("library", "", std::string("name=\"hashdb\" version=\"") + hashdb_version() + "\"",false);
#endif
#ifdef SQLITE_VERSION
    xmlout("library", "", "name=\"sqlite\" version=\"" SQLITE_VERSION "\" source_id=\"" SQLITE_SOURCE_ID "\"",false);
#endif
#ifdef HAVE_GNUEXIF
    // gnuexif does not have a programmatically obtainable version.
    xmlout("library","","name=\"gnuexif\" version=\"?\"",false);
#endif
#ifdef GIT_COMMIT
    xmlout("git", "", "commit=\"" GIT_COMMIT "\"",false);
#endif
    pop();
}
