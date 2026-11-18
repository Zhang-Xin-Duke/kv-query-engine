#pragma once
#include "runtime.h"
namespace index_engine {
class DirBased {
 std::string dir_;
 public:
 DirBased()=default;explicit DirBased(std::string dir):dir_(std::move(dir)){}
 const std::string& dir()const{return dir_;}void set_dir(const std::string& dir){dir_=dir;}
 std::string FilePath(const std::string& name)const{return (std::filesystem::path(dir_)/name).string();}
 int OpenForRead(const std::string& name,int64_t& size)const{
  std::error_code error;size=std::filesystem::file_size(FilePath(name),error);if(error)return -1;
  return ::open(FilePath(name).c_str(),O_RDONLY);
 }
 bool ReadFromFD(int fd,char* out,int64_t size)const{
  while(size>0){auto n=::read(fd,out,size);if(n<=0)return false;out+=n;size-=n;}return true;
 }
 bool ReadFile(const std::string& name,std::string& out){
  std::ifstream in(FilePath(name),std::ios::binary);if(!in)return false;
  out.assign(std::istreambuf_iterator<char>(in),{});return !in.bad();
 }
 friend std::ostream& operator<<(std::ostream& out,const DirBased& d){return out<<d.dir_;}
};
} // namespace index_engine
