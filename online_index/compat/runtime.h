#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "google/protobuf/io/coded_stream.h"
#include "proto/snapshot_meta.pb.h"
namespace portable {
struct Log {
 bool fatal=false;
 explicit Log(bool f=false):fatal(f){}
 template<class T> Log& operator<<(const T&) {return *this;}
 ~Log(){if(fatal)std::abort();}
};
}
#define LITE_LOG(level) portable::Log()
#undef LOG
#define LOG(level) portable::Log()
#undef PLOG
#define PLOG(level) portable::Log()
#undef VLOG
#define VLOG(level) portable::Log()
#define LOG_BY_INTERVAL(level, seconds) portable::Log()
#define CHECK(ok) portable::Log(!(ok))
#define DCHECK(ok) CHECK(ok)
#define PCHECK(ok) CHECK(ok)
#define ALLOW_UNUSED [[maybe_unused]]
#define DEFINE_TYPE_NAME(...) const std::string typename_ = #__VA_ARGS__
#define DISALLOW_COPY_AND_ASSIGN(T) T(const T&)=delete;T& operator=(const T&)=delete
#define DISALLOW_ASSIGN(T) T& operator=(const T&)=delete
#define DECLARE_double(n) extern double FLAGS_##n
#define DECLARE_int32(n) extern int32_t FLAGS_##n
#define DECLARE_int64(n) extern int64_t FLAGS_##n
#define DECLARE_bool(n) extern bool FLAGS_##n
#define DECLARE_string(n) extern std::string FLAGS_##n
namespace bthread {using Mutex=std::mutex;}
using bthread_t=uint64_t;
inline int bthread_start_background(bthread_t*, const void*, void*(*fn)(void*),void* arg) {
 try {std::thread([=]{fn(arg);}).detach();return 0;}catch(...){return -1;}
}
inline int bthread_usleep(uint64_t us){std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
namespace butil {
using StringPiece=std::string_view;
inline int64_t gettimeofday_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
class Timer {using Clock=std::chrono::steady_clock;Clock::time_point begin_;public:enum {STARTED};Timer(int=STARTED){start();}void start(){begin_=Clock::now();}void stop(){}int64_t n_elapsed()const{return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin_).count();}int64_t m_elapsed()const{return n_elapsed()/1000000;}};
template<class T>T* get_object(){return new T;}
template<class T>void return_object(T* p){delete p;}
inline uint64_t find_power2(uint64_t n){uint64_t p=1;while(p<n)p*=2;return p;}
inline std::string string_printf(const char* fmt,...){va_list args;va_start(args,fmt);va_list copy;va_copy(copy,args);int n=vsnprintf(nullptr,0,fmt,copy);va_end(copy);std::string out(n+1,'\0');vsnprintf(out.data(),out.size(),fmt,args);va_end(args);out.resize(n);return out;}
template<class...T>void string_printf(std::string* out,const char* fmt,T...args){*out=string_printf(fmt,args...);}
class fd_guard{int fd_;public:explicit fd_guard(int fd):fd_(fd){}~fd_guard(){if(fd_>=0)::close(fd_);}operator int()const{return fd_;}};
template<class K,class V>class FlatMap: public absl::flat_hash_map<K,V>{public:bool init(size_t){return true;}V* seek(const K&k){auto it=this->find(k);return it==this->end()?nullptr:&it->second;}};
using FilePath=std::filesystem::path;
inline bool DeleteFile(const FilePath& p,bool recursive){return recursive?std::filesystem::remove_all(p)>0:std::filesystem::remove(p);}
}
