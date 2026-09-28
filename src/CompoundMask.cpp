#include "CompoundMask.h"
#include "WicImage.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <queue>
#include <vector>
#include <cctype>
#include <stdexcept>
static std::string safe(std::string s){for(char& c:s)if(!std::isalnum((unsigned char)c)&&c!='_'&&c!='-')c='_';return s;}
static std::string esc(const std::string& s){std::string o;for(char c:s){if(c=='"'||c=='\\')o+='\\';if((unsigned char)c>=32)o+=c;}return o;}
void exportCompoundMask(const std::filesystem::path& folder,const std::string& label,const Mask& mask,const std::string& provenance,float confidence){
 if(mask.empty())throw std::runtime_error("Invalid mask dimensions");
 std::filesystem::create_directories(folder);
 const int w=mask.width,h=mask.height;const size_t n=mask.pixels.size();
 std::vector<unsigned char> seen(n,0);std::vector<size_t> queue;queue.reserve(4096);
 std::ofstream f(folder/(safe(label)+".components.json"));if(!f)throw std::runtime_error("Cannot create component metadata");
 f<<"{\"schema\":\"vims.compound-mask/1\",\"parent\":\""<<esc(label)<<"\",\"width\":"<<w<<",\"height\":"<<h<<",\"provenance\":\""<<esc(provenance)<<"\",\"confidence\":"<<confidence<<",\"connectivity\":8,\"components\":[";
 bool first=true;unsigned id=0;
 for(size_t start=0;start<n;++start){if(!mask.pixels[start]||seen[start])continue;
 queue.clear();queue.push_back(start);seen[start]=1;int x0=w,y0=h,x1=0,y1=0;
 for(size_t head=0;head<queue.size();++head){size_t v=queue[head];int x=int(v%w),y=int(v/w);x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
 for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){if(!dx&&!dy)continue;int xx=x+dx,yy=y+dy;if(xx<0||yy<0||xx>=w||yy>=h)continue;size_t j=size_t(yy)*w+xx;if(mask.pixels[j]&&!seen[j]){seen[j]=1;queue.push_back(j);}}
 }
 ++id;std::string name=safe(label)+"_component_"+std::to_string(id);
 Mask part;part.width=w;part.height=h;part.pixels.assign(n,0);for(size_t v:queue)part.pixels[v]=mask.pixels[v];
 saveMaskPngWic(folder/(name+".png"),part);
 if(!first)f<<',';first=false;f<<"{\"id\":\""<<name<<"\",\"pixels\":"<<queue.size()<<",\"bbox\":["<<x0<<','<<y0<<','<<x1<<','<<y1<<"],\"png\":\""<<name<<".png\"}";
 }
 f<<"],\"component_count\":"<<id<<"}\n";
}
