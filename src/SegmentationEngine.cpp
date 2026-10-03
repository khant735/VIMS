#include "SegmentationEngine.h"
#include "ColourShadowRefiner.h"
#include "FaceView.h"
#include "MaskOrientation.h"
#include "HiddenProjection.h"
#include "CrashLog.h"
#include "WicImage.h"
#include "LearningStore.h"
#include <windows.h>
#include <onnxruntime_c_api.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <iomanip>
#include <chrono>
#include <cmath>
namespace {const std::vector<std::string> ade={"wall","building","sky","floor","tree","ceiling","road","bed","windowpane","grass","cabinet","sidewalk","person","earth","door","table","mountain","plant","curtain","chair","car","water","painting","sofa","shelf","house","sea","mirror","rug","field","armchair","seat","fence","desk","rock","wardrobe","lamp","bathtub","railing","cushion","base","box","column","signboard","chest of drawers","counter","sand","sink","skyscraper","fireplace","refrigerator","grandstand","path","stairs","runway","case","pool table","pillow","screen door","stairway","river","bridge","bookcase","blind","coffee table","toilet","flower","book","hill","bench","countertop","stove","palm","kitchen island","computer","swivel chair","boat","bar","arcade machine","hovel","bus","towel","light","truck","tower","chandelier","awning","streetlight","booth","television receiver","airplane","dirt track","apparel","pole","land","bannister","escalator","ottoman","bottle","buffet","poster","stage","van","ship","fountain","conveyer belt","canopy","washer","plaything","swimming pool","stool","barrel","basket","waterfall","tent","bag","minibike","cradle","oven","ball","food","step","tank","trade name","microwave","pot","animal","bicycle","lake","dishwasher","screen","blanket","sculpture","hood","sconce","vase","traffic light","tray","ashcan","fan","pier","crt screen","plate","monitor","bulletin board","shower","radiator","glass","clock","flag"};const std::vector<std::string> clothes={"Background","Hat","Hair","Sunglasses","Upper-clothes","Skirt","Pants","Dress","Belt","Left-shoe","Right-shoe","Face","Left-leg","Right-leg","Left-arm","Right-arm","Bag","Scarf"};
void ortck(const OrtApi*a,OrtStatus*s){if(s){std::string m=a->GetErrorMessage(s);a->ReleaseStatus(s);throw std::runtime_error(m);}}
Mask classMask(const std::vector<uint16_t>&lab,int w,int h,const std::vector<std::string>&names,std::initializer_list<const char*> wanted){std::set<int> ids;for(auto q:wanted){auto it=std::find(names.begin(),names.end(),q);if(it!=names.end())ids.insert((int)(it-names.begin()));}Mask m{w,h,std::vector<uint8_t>(size_t(w)*h)};for(size_t i=0;i<lab.size();++i)m.pixels[i]=ids.count(lab[i])?255:0;return m;}
Mask mor(const Mask&a,const Mask&b){Mask m{a.width,a.height,std::vector<uint8_t>(a.pixels.size())};for(size_t i=0;i<m.pixels.size();++i)m.pixels[i]=a.pixels[i]|b.pixels[i];return m;} bool any(const Mask&m){return std::any_of(m.pixels.begin(),m.pixels.end(),[](auto x){return x;});}
static std::string slug(std::string s){for(char&c:s){if((c>='A'&&c<='Z'))c=char(c-'A'+'a');else if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')))c='_';}while(s.find("__")!=std::string::npos)s.replace(s.find("__"),2,"_");return s;}
static std::vector<MaskComponent> componentsOf(const Mask&m,const std::string&base){
 std::vector<MaskComponent> out;if(m.pixels.empty())return out;const int W=m.width,H=m.height;std::vector<uint8_t>seen(m.pixels.size());std::queue<size_t>q;size_t ordinal=0;
 for(size_t st=0;st<m.pixels.size();++st)if(m.pixels[st]&&!seen[st]){std::vector<size_t> px;q.push(st);seen[st]=1;int minx=W,miny=H,maxx=0,maxy=0;
  while(!q.empty()){size_t i=q.front();q.pop();px.push_back(i);int x=int(i%W),y=int(i/W);minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y);
   if(x>0){size_t j=i-1;if(m.pixels[j]&&!seen[j]){seen[j]=1;q.push(j);}}if(x+1<W){size_t j=i+1;if(m.pixels[j]&&!seen[j]){seen[j]=1;q.push(j);}}
   if(y>0){size_t j=i-W;if(m.pixels[j]&&!seen[j]){seen[j]=1;q.push(j);}}if(y+1<H){size_t j=i+W;if(m.pixels[j]&&!seen[j]){seen[j]=1;q.push(j);}}
  }
  Mask cm{W,H,std::vector<uint8_t>(m.pixels.size())};for(auto i:px)cm.pixels[i]=255;int bw=maxx-minx+1,bh=maxy-miny+1;bool thin=std::min(bw,bh)<=3||px.size()<size_t(std::max(bw,bh)*3);
  MaskComponent c;c.instanceId=slug(base)+"_"+std::to_string(++ordinal);c.mask=std::move(cm);c.pixelArea=px.size();c.thinStructure=thin;out.push_back(std::move(c));
 }return out;
}
void add(std::vector<NamedMask>&o,const char*n,const Mask&m,const char*cat="General",const char*parent="",const char*source="semantic",float conf=1.0f){if(any(m)){NamedMask nm;nm.name=n;nm.mask=m;nm.category=cat;nm.parent=parent;nm.source=source;nm.confidence=conf;nm.provenance=(std::string(source).find(".onnx")!=std::string::npos)?MaskProvenance::SpecialistModel:MaskProvenance::SemanticModel;nm.components=componentsOf(m,n);o.push_back(std::move(nm));}}


static size_t maskArea(const Mask&m){size_t n=0;for(auto v:m.pixels)n+=v!=0;return n;}
static std::vector<Mask> significantInstances(const Mask&m){
 auto cc=componentsOf(m,"instance"); std::vector<Mask> out; const size_t whole=maskArea(m);
 const size_t minArea=std::max<size_t>(64,size_t(m.width)*size_t(m.height)/10000);
 for(auto&c:cc)if(maskArea(c.mask)>=minArea)out.push_back(std::move(c.mask));
 std::stable_sort(out.begin(),out.end(),[&](const Mask&a,const Mask&b){
  const auto aa=maskArea(a),bb=maskArea(b); if(aa!=bb)return aa>bb;
  auto centre=[&](const Mask&x){double sx=0,sy=0,n=0;for(int y=0;y<x.height;++y)for(int xx=0;xx<x.width;++xx)if(x.pixels[size_t(y)*x.width+xx]){sx+=xx;sy+=y;n++;}if(!n)return 1e30;double dx=sx/n-(x.width-1)*.5,dy=sy/n-(x.height-1)*.5;return dx*dx+dy*dy;};
  return centre(a)<centre(b);
 }); return out;
}
static Mask intersectMask(const Mask&a,const Mask&b){
 Mask r{a.width,a.height,std::vector<uint8_t>(a.pixels.size())};if(a.width!=b.width||a.height!=b.height)return r;
 for(size_t i=0;i<r.pixels.size();++i)r.pixels[i]=(a.pixels[i]&&b.pixels[i])?255:0;return r;
}
static void addInstances(std::vector<NamedMask>&v,const std::string&kind,const Mask&combined,const std::vector<std::pair<std::string,Mask>>&children={}){
 auto inst=significantInstances(combined);for(size_t i=0;i<inst.size();++i){
  const std::string id=(i==0?"main_"+kind:kind+"_"+(i<10?"0":"")+std::to_string(i));
  add(v,id.c_str(),inst[i],"Instance",kind.c_str(),"connected-component instance separation",0.72f);
  for(const auto&ch:children){auto cm=intersectMask(ch.second,inst[i]);if(maskArea(cm)){auto child=id+"_"+slug(ch.first);add(v,child.c_str(),cm,"Instance detail",id.c_str(),"instance-owned semantic detail",0.70f);}}
 }
}
static std::string canonicalLeaf(const std::string& n){
 std::string x=n; auto p=x.rfind(" > "); if(p!=std::string::npos)x=x.substr(p+3);
 for(char&c:x)c=(char)tolower((unsigned char)c);
 if(x=="hair")return "human.hair"; if(x=="face")return "human.face"; if(x=="hat")return "clothing.hat";
 if(x=="sunglasses"||x=="eye-glass")return "accessory.eyewear"; return slug(x);
}
static double maskIoU(const Mask&a,const Mask&b){
 if(a.width!=b.width||a.height!=b.height)return 0;size_t in=0,un=0;
 for(size_t i=0;i<a.pixels.size();++i){bool A=a.pixels[i]!=0,B=b.pixels[i]!=0;in+=A&&B;un+=A||B;}return un?double(in)/double(un):0;
}
static int provenanceRank(const NamedMask&m){
 if(m.provenance==MaskProvenance::SpecialistModel)return 4;
 if(m.provenance==MaskProvenance::SemanticModel)return 3;
 if(m.provenance==MaskProvenance::LandmarkDerived)return 2;
 return 1;
}
static void consolidateLeafDuplicates(std::vector<NamedMask>&v){
 std::vector<uint8_t> drop(v.size());
 for(size_t i=0;i<v.size();++i)for(size_t j=i+1;j<v.size();++j){
  if(drop[i]||drop[j])continue;if(canonicalLeaf(v[i].name)!=canonicalLeaf(v[j].name))continue;
  double ov=maskIoU(v[i].mask,v[j].mask);if(ov<0.45)continue;
  size_t keep=i,lose=j;if(provenanceRank(v[j])>provenanceRank(v[i])||(provenanceRank(v[j])==provenanceRank(v[i])&&v[j].confidence>v[i].confidence)){keep=j;lose=i;}
  // Preserve the stronger semantic mask; union only close boundary-support pixels from the weaker source.
  Mask merged=v[keep].mask;for(size_t k=0;k<merged.pixels.size();++k)if(v[lose].mask.pixels[k])merged.pixels[k]=255;
  v[keep].mask=std::move(merged);v[keep].components=componentsOf(v[keep].mask,v[keep].name);
  v[keep].source += std::string("+consolidated:")+v[lose].source;drop[lose]=1;
 }
 std::vector<NamedMask> out;out.reserve(v.size());for(size_t i=0;i<v.size();++i)if(!drop[i])out.push_back(std::move(v[i]));v.swap(out);
}
// Constrain small specialist labels to the corresponding body region before displaying them.
static bool bounds(const Mask& m,int& x0,int& y0,int& x1,int& y1){
 x0=m.width;y0=m.height;x1=-1;y1=-1;
 for(int y=0;y<m.height;++y)for(int x=0;x<m.width;++x)if(m.pixels[size_t(y)*m.width+x]){
  x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);
 }return x1>=x0;
}
static Mask headRegion(const Mask& predicted,const Mask& face,const char* part,const FaceView* view=nullptr){
 Mask out=predicted;std::fill(out.pixels.begin(),out.pixels.end(),0);
 int x0,y0,x1,y1;if(!bounds(face,x0,y0,x1,y1))return out;
 const int fw=x1-x0+1,fh=y1-y0+1;
 int left=std::max(0,x0-fw/2),right=std::min(face.width-1,x1+fw/2);
 int top=std::max(0,y0-fh/3),bottom=std::min(face.height-1,y1+fh/4);
 if(std::string(part)=="Neck"){left=x0+fw/5;right=x1-fw/5;top=y0+fh*72/100;}
 if(std::string(part)=="Hair")bottom=y0+fh*53/100;
 else if(std::string(part)=="Left-ear"||std::string(part)=="Right-ear")bottom=y0+fh*75/100;
 else if(std::string(part)!="Neck"){
  left=std::max(0,x0-fw/10);right=std::min(face.width-1,x1+fw/10);
  top=y0;bottom=y1;
 }
 if(view&&std::abs(view->roll)>.07){
  const std::string label=part;
  for(int y=std::max(0,y0-fh/3);y<=std::min(face.height-1,y1+fh/4);++y)
   for(int x=std::max(0,x0-fw/2);x<=std::min(face.width-1,x1+fw/2);++x){
    const auto [u,v]=view->coordinates(x+.5,y+.5);
    const bool in=label=="Hair"?v>=-.33&&v<=.53&&u>=-.5&&u<=1.5:
     label=="Neck"?v>=.72&&v<=1.25&&u>=.20&&u<=.80:
     label=="Left-ear"||label=="Right-ear"?v>=-.3&&v<=.75&&u>=-.5&&u<=1.5:
     v>=0&&v<=1&&u>=-.1&&u<=1.1;
    if(in){const size_t i=size_t(y)*face.width+x;out.pixels[i]=predicted.pixels[i];}
   }
 }else for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){size_t i=size_t(y)*face.width+x;out.pixels[i]=predicted.pixels[i];}
 return out;
}
// Landmark fallback across frontal and moderately turned faces. Candidates are constrained by the
// detected face, and only local colour/contrast evidence becomes mask pixels.
// This is deliberately lower confidence than a face parsing model prediction.
static std::vector<std::pair<std::string,Mask>> deriveFaceParts(const ImageRGBA& im,
    const Mask& face,const Mask& person,const FaceView& view){
 std::vector<std::pair<std::string,Mask>> result;
 int x0,y0,x1,y1;if(!bounds(face,x0,y0,x1,y1))return result;
 const int fw=x1-x0+1,fh=y1-y0+1,W=im.width,H=im.height;
 if(fw<45||fh<55||maskArea(face)<size_t(fw*fh/8))return result;
 // Estimate local skin brightness and colour from the centre of the face.
 double skinL=0,skinR=0,skinG=0,skinB=0;int count=0;
 for(int y=y0+fh*28/100;y<std::min(H,y0+fh*38/100);++y)
  for(int x=x0+fw*38/100;x<std::min(W,x0+fw*62/100);++x){
   size_t i=size_t(y)*W+x;if(!face.pixels[i])continue;
   auto p=&im.pixels[i*4];if(p[3]<64)continue;
   skinR+=p[0];skinG+=p[1];skinB+=p[2];skinL+=(p[0]*3+p[1]*6+p[2])/10.0;++count;
  }
 if(count<20)return result;
 skinL/=count;skinR/=count;skinG/=count;skinB/=count;
 auto part=[&](const char* name,double xa,double xb,double ya,double yb,int kind){
  const std::string label=name;
  if(view.profile&&(kind==0||kind==1)){
   if((label.find("Left ")!=std::string::npos&&!view.leftEye)||
      (label.find("Right ")!=std::string::npos&&!view.rightEye))return;
   const auto [u,v]=view.coordinates(view.eyeX,view.eyeY);
   const double shift=u-(xa+xb)*.5;xa+=shift;xb+=shift;
   if(kind==1){const double yshift=v-(ya+yb)*.5;ya+=yshift;yb+=yshift;}
   else {const double yshift=v-.09-(ya+yb)*.5;ya+=yshift;yb+=yshift;}
  }
  else if(std::abs(view.yawHint)>.20&&(kind==0||kind==1)){
   const bool left=label.find("Left ")!=std::string::npos;
   const bool valid=left?view.leftEye:view.rightEye;
   if(valid){
    const auto [u,v]=view.coordinates(left?view.leftX:view.rightX,left?view.leftY:view.rightY);
    const double shift=.7*(u-(xa+xb)*.5),vertical=.7*(v-(kind==0?.09:0)-(ya+yb)*.5);
    xa+=shift;xb+=shift;ya+=vertical;yb+=vertical;
   }
  }
  if(view.nose&&(view.profile||std::abs(view.yawHint)>.20)&&(kind==2||kind==3)){
   const auto [u,v]=view.coordinates(view.noseX,view.noseY);
   const double shift=u-(xa+xb)*.5;xa+=shift;xb+=shift;
   if(kind==2){const double yshift=v-(ya+yb)*.5;ya+=yshift;yb+=yshift;}
   else {const double yshift=v+.16-(ya+yb)*.5;ya+=yshift;yb+=yshift;}
  }
  Mask seed{W,H,std::vector<uint8_t>(size_t(W)*H)};
  const int left=std::max(0,x0-fw/3),right=std::min(W-1,x1+fw/3);
  const int top=std::max(0,y0-fh/3),bottom=std::min(H-1,y1+fh/3);
  auto inside=[&](int px,int py){const auto [u,v]=view.coordinates(px+.5,py+.5);
   return u>=xa&&u<=xb&&v>=ya&&v<=yb;};
  int evidence=0;
  for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){size_t i=size_t(y)*W+x;
   if(!inside(x,y)||!face.pixels[i])continue;auto p=&im.pixels[i*4];if(p[3]<64)continue;
   const double lum=(p[0]*3+p[1]*6+p[2])/10.0;
   const bool dark=lum<skinL*(kind==3?0.83:kind==2?0.62:0.69);
   const bool lightEye=(kind==1&&p[2]>skinB+30&&lum>skinL*0.7);
   if(dark||lightEye){seed.pixels[i]=255;++evidence;}
  }
  if(evidence<std::max(8,int(fw*fh*(xb-xa)*(yb-ya)/180)))return;
  Mask out=seed;
  const int rx=std::max(2,fw*(kind==2?8:kind==3?4:kind==1?4:2)/100);
  const int ry=std::max(2,fh*(kind==2?5:kind==3?2:kind==1?2:1)/100);
  for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){size_t i=size_t(y)*W+x;
   if(!inside(x,y)||!face.pixels[i]||im.pixels[i*4+3]<64)continue;
   for(int dy=-ry;dy<=ry&&!out.pixels[i];++dy){int yy=y+dy;if(yy<top||yy>bottom)continue;
    for(int dx=-rx;dx<=rx;++dx){int xx=x+dx;if(xx<left||xx>right)continue;
     if(dx*dx*ry*ry+dy*dy*rx*rx<=rx*rx*ry*ry&&seed.pixels[size_t(yy)*W+xx]){out.pixels[i]=255;break;}
    }
   }
  }
  result.emplace_back(name,std::move(out));
 };
 // Anatomical left is on the viewer's right in a frontal face.
 part("Human > Head > Face > Right Eyebrow",.15,.56,.03,.17,0);
 part("Human > Head > Face > Left Eyebrow",.56,.96,.05,.21,0);
 part("Human > Head > Face > Right Eye",.17,.55,.16,.29,1);
 part("Human > Head > Face > Left Eye",.55,.96,.18,.31,1);
 // The nasal bridge begins between/below the brows and runs to the nasal tip.
 // Keep the parent Nose mask, but publish a narrower bridge/root prior separately so
 // eyelid pixels cannot become the app's only interpretation of "nose".
 part("Human > Head > Face > Nose",.39,.68,.20,.52,2);
 part("Human > Head > Face > Nose > Bridge",.43,.64,.18,.39,2);
 part("Human > Head > Face > Nose > Tip",.39,.69,.36,.50,2);
 part("Human > Head > Face > Mouth",.17,.88,.50,.68,3);
 // Side protrusions belong to the head. Require colour close to the face and
 // visible pixels outside its boundary, avoiding invented ears on plain photos.
 auto ear=[&](const char* name,bool viewerLeft){
  Mask m{W,H,std::vector<uint8_t>(size_t(W)*H)};
  int l=std::clamp(x0-int(fw*.28),0,W-1),r=std::clamp(x0+int(fw*.19),0,W-1);
  if(!viewerLeft){l=std::clamp(x1-int(fw*.17),0,W-1);r=std::clamp(x1+int(fw*.28),0,W-1);}
  int t=std::clamp(y0-int(fh*.09),0,H-1),b=std::clamp(y0+int(fh*.40),0,H-1);
  int outer=0;
  for(int y=t;y<=b;++y)for(int x=l;x<=r;++x){size_t i=size_t(y)*W+x;
   if(face.pixels[i]||(!person.pixels[i]&&im.pixels[i*4+3]<192))continue;
   auto p=&im.pixels[i*4];if(p[3]<128)continue;
   double diff=abs(p[0]-skinR)+abs(p[1]-skinG)+abs(p[2]-skinB);
   if(diff>std::max(115.0,skinL*.85))continue;
   m.pixels[i]=255;if(viewerLeft?x<x0-fw/10:x>x1+fw/10)++outer;
  }
  if(outer>std::max(15,fw*fh/700))result.emplace_back(name,std::move(m));
 };
 if(!view.profile&&std::abs(view.roll)<.10){ear("Human > Head > Right Ear",true);ear("Human > Head > Left Ear",false);}
 return result;
}
static Mask neckFromFaceColour(const ImageRGBA& im,const Mask& face,const Mask& predicted,const FaceView& view){
 Mask out=headRegion(predicted,face,"Neck",&view);
 int x0,y0,x1,y1;if(!bounds(face,x0,y0,x1,y1))return out;
 const int fw=x1-x0+1,fh=y1-y0+1;
 // Derive a conservative centre patch when the face model confuses the neck with legs.
 if(maskArea(out)>size_t(std::max(8,fw*fh/200)))return out;
 if(std::abs(view.roll)>.10)return out;
 const int sx0=x0+fw*35/100,sx1=x1-fw*35/100;
 const int sy0=y0+fh*58/100,sy1=y0+fh*78/100;
 long rgb[3]={},n=0;
 for(int y=sy0;y<=sy1;++y)for(int x=sx0;x<=sx1;++x){size_t i=size_t(y)*im.width+x;
  if(face.pixels[i]){auto* q=&im.pixels[i*4];for(int c=0;c<3;++c)rgb[c]+=q[c];++n;}
 }
 if(n<8)return out;
 const int top=y0+fh*82/100,bottom=std::min(im.height-1,y1+fh/7);
 for(int y=top;y<=bottom;++y)for(int x=x0+fw/5;x<=x1-fw/5;++x){size_t i=size_t(y)*im.width+x;auto* q=&im.pixels[i*4];
  int d=abs(int(q[0])-int(rgb[0]/n))+abs(int(q[1])-int(rgb[1]/n))+abs(int(q[2])-int(rgb[2]/n));
  if(q[3]&&d<65)out.pixels[i]=255;
 }
 return out;
}
static std::pair<Mask,Mask> separateShoes(const Mask& leftModel,const Mask& rightModel){
 Mask left=leftModel,right=rightModel;
 std::fill(left.pixels.begin(),left.pixels.end(),0);std::fill(right.pixels.begin(),right.pixels.end(),0);
 int lx0,ly0,lx1,ly1,rx0,ry0,rx1,ry1;
 if(!bounds(leftModel,lx0,ly0,lx1,ly1)||!bounds(rightModel,rx0,ry0,rx1,ry1))return {leftModel,rightModel};
 // Anatomical left is viewer right; the horizontal split assigns stray toe pixels to the correct shoe.
 int cxL=(lx0+lx1)/2,cxR=(rx0+rx1)/2;
 int split=(cxL+cxR)/2;
 if(cxL<cxR){ // Correct a reversed or noisy label based on image positions.
  cxL=(rx0+rx1)/2;cxR=(lx0+lx1)/2;split=(cxL+cxR)/2;
 }
 for(size_t i=0;i<left.pixels.size();++i)if(leftModel.pixels[i]||rightModel.pixels[i]){
  if(int(i%left.width)>split)left.pixels[i]=255;else right.pixels[i]=255;
 }
 return {left,right};
}
static std::pair<Mask,Mask> trouserLegs(const Mask& pants){
 Mask left=pants,right=pants;std::fill(left.pixels.begin(),left.pixels.end(),0);std::fill(right.pixels.begin(),right.pixels.end(),0);
 int x0,y0,x1,y1;if(!bounds(pants,x0,y0,x1,y1))return {left,right};
 const int h=y1-y0+1,w=x1-x0+1;if(h<pants.height/12||w<pants.width/12)return {left,right};
 // Find a central gap low on the garment, excluding the continuous hip section.
 int split=(x0+x1)/2;int bestGap=0;
 for(int y=y0+h*55/100;y<=y0+h*90/100;++y){
  int l=-1,r=-1;for(int x=split;x>=x0;--x)if(pants.pixels[size_t(y)*pants.width+x]){l=x;break;}
  for(int x=split+1;x<=x1;++x)if(pants.pixels[size_t(y)*pants.width+x]){r=x;break;}
  if(l>=0&&r>=0&&r-l-1>bestGap){bestGap=r-l-1;split=(l+r)/2;}
 }
 if(bestGap<std::max(3,w/40))return {left,right};
 // Only label the separate lower parts; the hips remain in the existing Pants mask.
 const int start=y0+h*28/100;
 for(int y=start;y<=y1;++y)for(int x=x0;x<=x1;++x){size_t i=size_t(y)*pants.width+x;
  if(pants.pixels[i]) (x>split?left:right).pixels[i]=255; // Anatomical left is viewer right.
 }
 if(maskArea(left)<maskArea(pants)/10||maskArea(right)<maskArea(pants)/10){
  std::fill(left.pixels.begin(),left.pixels.end(),0);std::fill(right.pixels.begin(),right.pixels.end(),0);
 }
 return {left,right};
}
static Mask growAccessory(const ImageRGBA& im,const Mask& seed,const Mask& garment,
                          const Mask& skin,bool belt,const Mask* excluded=nullptr){
 Mask out=seed;int x0,y0,x1,y1;if(!bounds(seed,x0,y0,x1,y1))return out;
 int gx0,gy0,gx1,gy1;if(!bounds(garment,gx0,gy0,gx1,gy1))return out;
 const int W=im.width,H=im.height;
 const int xmin=std::max(0,gx0-12),xmax=std::min(W-1,gx1+12);
 const int mid=(gx0+gx1)/2;
 int ymin,ymax;
 if(belt){int margin=std::max(24,H/12);ymin=std::max(0,y0-margin);ymax=std::min(H-1,y1+margin);}
 else {ymin=std::max(0,gy0-12);ymax=std::min(H-1,y1+std::max(12,H/40));}
 if(xmin>xmax||ymin>ymax)return out;
 // A small palette sampled across the original mask tolerates woven or shaded fabric.
 std::vector<size_t> samples;size_t seedArea=maskArea(seed),stride=std::max<size_t>(1,seedArea/192),ordinal=0;
 for(size_t i=0;i<seed.pixels.size();++i)if(seed.pixels[i]&&ordinal++%stride==0){const auto* q=&im.pixels[i*4];
  if(int(q[0])+q[1]+q[2]<(belt?450:430))samples.push_back(i);}
 if(samples.empty())return out;
 // Precompute the palette distance once for quantised RGB; matching then costs one lookup per pixel.
 std::vector<uint8_t> palette(32*32*32);
 for(int r=0;r<32;++r)for(int g=0;g<32;++g)for(int b=0;b<32;++b){
  int best=999;for(size_t j:samples){const auto* p=&im.pixels[j*4];
   int d=abs(r*8+4-int(p[0]))+abs(g*8+4-int(p[1]))+abs(b*8+4-int(p[2]));
   best=std::min(best,d);
  }
  palette[(r*32+g)*32+b]=best<=(belt?65:55);
 }
 auto matches=[&](size_t i){if(skin.pixels[i]||(excluded&&excluded->pixels[i])||im.pixels[i*4+3]==0)return false;
  const auto* q=&im.pixels[i*4];
  if(int(q[0])+q[1]+q[2]>(belt?450:430))return false;
  return palette[((q[0]>>3)*32+(q[1]>>3))*32+(q[2]>>3)]!=0;
 };
 std::queue<size_t> queue;
 for(int y=ymin;y<=ymax;++y)for(int x=xmin;x<=xmax;++x){size_t i=size_t(y)*W+x;if(seed.pixels[i])queue.push(i);}
 auto offer=[&](int x,int y){if(x<xmin||x>xmax||y<ymin||y>ymax)return;
  size_t i=size_t(y)*W+x;if(out.pixels[i]||!matches(i))return;
  if(!belt){
   if(!garment.pixels[i])return;
   const int allowance=std::max(10,(x1-x0+1)/6);
   const int mirrorL=2*mid-x1,mirrorR=2*mid-x0;
   if(!(x>=x0-allowance&&x<=x1+allowance)&&
      !(x>=mirrorL-allowance&&x<=mirrorR+allowance))return;
  }
  out.pixels[i]=255;queue.push(i);
 };
 while(!queue.empty()){size_t i=queue.front();queue.pop();int x=int(i%W),y=int(i/W);
  offer(x-1,y);offer(x+1,y);offer(x,y-1);offer(x,y+1);
 }
 if(!belt){
  // The two scarf panels can be disconnected by the shirt. Seek a mirrored
  // material patch only on the opposite half, then grow it within the garment.
  const bool seedRight=(x0+x1)/2>=mid;
  size_t best=0;int bx=-1,by=-1;
  const int target=std::clamp(gx0+gx1-(x0+x1)/2,xmin,xmax);
  for(int y=ymin;y<=ymax;++y)for(int x=xmin;x<=xmax;++x){
   if((x>=mid)==seedRight||abs(x-target)>std::max(12,(x1-x0+1)/2)||!garment.pixels[size_t(y)*W+x])continue;
   size_t i=size_t(y)*W+x;if(!matches(i))continue;
   size_t score=size_t(abs(x-target)+abs(y-(y0+y1)/2));
   if(bx<0||score<best){best=score;bx=x;by=y;}
  }
  if(bx>=0){size_t i=size_t(by)*W+bx;out.pixels[i]=255;queue.push(i);
   while(!queue.empty()){size_t n=queue.front();queue.pop();int x=int(n%W),y=int(n/W);
    if((x>=mid)==seedRight)continue;
    offer(x-1,y);offer(x+1,y);offer(x,y-1);offer(x,y+1);
   }
  }
  // Follow narrow dark ties between the two scarf panels. Restrict this pass
  // to the upper centre so unrelated dark clothing cannot be absorbed.
  const int cx=(gx0+gx1)/2, span=std::max(12,(gx1-gx0)/5);
  const int tieTop=std::max(ymin,y0-std::max(8,H/60));
  const int tieBottom=std::min(ymax,y0+std::max(24,(y1-y0)*2/3));
  for(int y=tieTop;y<=tieBottom;++y)for(int x=std::max(xmin,cx-span);x<=std::min(xmax,cx+span);++x){
   size_t i=size_t(y)*W+x;if(out.pixels[i])queue.push(i);
  }
  auto tieOffer=[&](int x,int y,size_t from){
   if(x<std::max(xmin,cx-span)||x>std::min(xmax,cx+span)||y<tieTop||y>tieBottom)return;
   size_t i=size_t(y)*W+x;if(out.pixels[i]||!garment.pixels[i]||skin.pixels[i])return;
   const auto* q=&im.pixels[i*4];const auto* p=&im.pixels[from*4];
   if(q[3]==0||int(q[0])+q[1]+q[2]>290||
      abs(int(q[0])-p[0])+abs(int(q[1])-p[1])+abs(int(q[2])-p[2])>135)return;
   out.pixels[i]=255;queue.push(i);
  };
  while(!queue.empty()){size_t i=queue.front();queue.pop();int x=int(i%W),y=int(i/W);
   tieOffer(x-1,y,i);tieOffer(x+1,y,i);tieOffer(x,y-1,i);tieOffer(x,y+1,i);
  }
 }
 return out;
}
// Publish one canonical mask per detected region and attach it to a navigable tree.
// Category-only nodes are created by the UI; they do not duplicate pixel masks.
static void organizeHierarchy(AnalysisResult& result){
 auto oldRaw=std::move(result.rawMasks),old=std::move(result.masks);
 if(old.size()!=oldRaw.size()){result.rawMasks=std::move(oldRaw);result.masks=std::move(old);return;}
 const size_t n=old.size();std::vector<uint8_t> used(n);
 const std::string person="People/Human/Person 1",head=person+"/Head";
 auto indexOf=[&](const std::string& name)->int{
  for(size_t i=0;i<n;++i){if(old[i].name==name)return int(i);}
  return -1;
 };
 auto put=[&](int i,const std::string& path){if(i<0||used[i])return;
  used[i]=1;old[i].name=path;oldRaw[i].name=path;
  auto sep=path.rfind('/');old[i].parent=oldRaw[i].parent=(sep==std::string::npos?"":path.substr(0,sep));
  result.masks.push_back(std::move(old[i]));result.rawMasks.push_back(std::move(oldRaw[i]));
 };
 const int mainPerson=indexOf("main_person")>=0?indexOf("main_person"):indexOf("People");
 put(mainPerson,person);
 // Build broad anatomical masks before moving the individual model outputs.
 const int face=indexOf("Face")>=0?indexOf("Face"):indexOf("Human > Head > Face");
 const int hairAnchor=indexOf("Human > Head > Hair")>=0?indexOf("Human > Head > Hair"):indexOf("Hair");
 const int neck=indexOf("Human > Neck");
 const int pants=indexOf("Pants");
 const int leftArm=indexOf("Left-arm"),rightArm=indexOf("Right-arm");
 auto makeRegion=[&](Mask m,const std::string& path,const char* source,float confidence){
  NamedMask item;item.name=path;item.parent=path.substr(0,path.rfind('/'));
  item.category="Human anatomy";item.source=source;item.mask=std::move(m);
  item.confidence=confidence;return item;
 };
 auto publishRegion=[&](Mask raw,Mask refined,const std::string& path,const char* source,float confidence){
  if(!any(raw)&&!any(refined))return;
  result.rawMasks.push_back(makeRegion(std::move(raw),path,source,confidence));
  result.masks.push_back(makeRegion(std::move(refined),path,source,confidence));
 };
 if(mainPerson>=0&&(face>=0||hairAnchor>=0)){
  Mask rawFace=face>=0?oldRaw[face].mask:Mask{oldRaw[mainPerson].mask.width,oldRaw[mainPerson].mask.height,std::vector<uint8_t>(oldRaw[mainPerson].mask.pixels.size())};
  Mask refinedFace=face>=0?old[face].mask:rawFace;
  for(const char* label:{"Human > Head > Face > Nose","Human > Head > Face > Left Eye",
        "Human > Head > Face > Right Eye","Human > Head > Face > Left Eyebrow",
        "Human > Head > Face > Right Eyebrow","Human > Head > Face > Mouth",
        "Human > Head > Face > Mouth > Upper Lip","Human > Head > Face > Mouth > Lower Lip",
        "Human > Head > Face > Mouth > Teeth"}){
   int j=indexOf(label);if(j<0)continue;
   for(size_t k=0;k<rawFace.pixels.size();++k){
    rawFace.pixels[k]|=oldRaw[j].mask.pixels[k];refinedFace.pixels[k]|=old[j].mask.pixels[k];
   }
  }
  Mask rawHead=rawFace,refinedHead=refinedFace;
  const char* hairSource=indexOf("Human > Head > Hair")>=0?"Human > Head > Hair":"Hair";
  for(const char* label:{"Human > Head > Left Ear","Human > Head > Right Ear",hairSource}){
   int j=indexOf(label);if(j<0)continue;
   for(size_t k=0;k<rawHead.pixels.size();++k){
    rawHead.pixels[k]|=oldRaw[j].mask.pixels[k];refinedHead.pixels[k]|=old[j].mask.pixels[k];
   }
  }
  // The source person silhouette is split around the head, neck, waist and
  // sides of the trunk. These are approximate regions, not extra model classes.
  auto parts=[&](bool refined)->std::array<Mask,4>{
   const auto& v=refined?old:oldRaw;
   const Mask& personMask=refined?result.masks.front().mask:result.rawMasks.front().mask;
   const Mask& headMask=refined?refinedHead:rawHead;
   Mask body=personMask,upper=personMask,torso=personMask,lower=personMask;
   std::fill(body.pixels.begin(),body.pixels.end(),0);
   std::fill(upper.pixels.begin(),upper.pixels.end(),0);
   std::fill(torso.pixels.begin(),torso.pixels.end(),0);
   std::fill(lower.pixels.begin(),lower.pixels.end(),0);
   const MaskFrame frame=estimateMaskFrame(personMask,true);
   const int W=personMask.width,H=personMask.height;
   auto range=[&](const Mask& mask){
    double u0=1e30,u1=-1e30,v0=1e30,v1=-1e30;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(mask.pixels[size_t(y)*W+x]){
     const auto [u,vert]=frame.coordinates(x+.5,y+.5);
     u0=std::min(u0,u);u1=std::max(u1,u);v0=std::min(v0,vert);v1=std::max(v1,vert);
    }
    return std::array<double,4>{u0,u1,v0,v1};
   };
   const auto headRange=range(headMask);
   const bool havePants=pants>=0&&any(v[pants].mask);
   const auto pantsRange=havePants?range(v[pants].mask):std::array<double,4>{};
   const double headBottom=std::clamp(headRange[3],0.0,.93);
   const double waist=havePants?std::clamp(pantsRange[2],headBottom+.01,.99):headBottom+(1-headBottom)*.67;
   const double upperEnd=headBottom+(waist-headBottom)*.43;
   const double centre=(headRange[0]+headRange[1])*.5;
   const double headHalf=(headRange[1]-headRange[0])*.5;
   const double torsoHalf=havePants?std::max((pantsRange[1]-pantsRange[0])*.6,headHalf*.75):std::max(.06,headHalf);
   for(int y=0;y<H;++y)for(int x=0;x<W;++x){const size_t k=size_t(y)*W+x;
    if(!personMask.pixels[k]||headMask.pixels[k]||(neck>=0&&v[neck].mask.pixels[k]))continue;
    body.pixels[k]=255;
    const auto [u,vert]=frame.coordinates(x+.5,y+.5);
    const bool arm=(leftArm>=0&&v[leftArm].mask.pixels[k])||
                   (rightArm>=0&&v[rightArm].mask.pixels[k])||
                   (vert<waist&&(u<centre-torsoHalf||u>centre+torsoHalf));
    if(vert>=waist){lower.pixels[k]=255;continue;}
    if(arm||vert<=upperEnd)upper.pixels[k]=255;
    else torso.pixels[k]=255;
   }
   return {std::move(body),std::move(upper),std::move(torso),std::move(lower)};
  };
  auto rawParts=parts(false),refinedParts=parts(true);
  publishRegion(std::move(rawHead),std::move(refinedHead),head,"union of face, scalp, ears and hair",0.78f);
  publishRegion(std::move(rawFace),std::move(refinedFace),head+"/Face","union of visible facial parts",0.82f);
  put(neck,person+"/Neck");
  publishRegion(std::move(rawParts[0]),std::move(refinedParts[0]),person+"/Body","approximate body silhouette",0.65f);
  publishRegion(std::move(rawParts[1]),std::move(refinedParts[1]),person+"/Body/Upper body","approximate chest, ribs and arm region",0.55f);
  publishRegion(std::move(rawParts[2]),std::move(refinedParts[2]),person+"/Body/Torso","approximate stomach and trunk region",0.55f);
  publishRegion(std::move(rawParts[3]),std::move(refinedParts[3]),person+"/Body/Lower body","approximate hips, waist and leg region",0.55f);
  if(face>=0)used[face]=1; // Face is now represented by its complete union.
 }
 for(const auto& [oldName,path]:std::initializer_list<std::pair<const char*,const char*>>{
  {"Human > Head > Face > Nose","/Nose"},{"Human > Head > Face > Left Eye","/Left eye"},
  {"Human > Head > Face > Right Eye","/Right eye"},{"Human > Head > Face > Left Eyebrow","/Left eyebrow"},
  {"Human > Head > Face > Right Eyebrow","/Right eyebrow"},{"Human > Head > Face > Mouth","/Mouth"},
  {"Human > Head > Face > Mouth > Upper Lip","/Mouth/Upper lip"},
  {"Human > Head > Face > Mouth > Lower Lip","/Mouth/Lower lip"},
  {"Human > Head > Face > Mouth > Teeth","/Mouth/Teeth"}})
  put(indexOf(oldName),head+"/Face"+path);
 put(indexOf("Human > Head > Left Ear"),head+"/Left ear");
 put(indexOf("Human > Head > Right Ear"),head+"/Right ear");
 int hair=indexOf("Human > Head > Hair");if(hair<0)hair=indexOf("Hair");
 put(hair,head+"/Hair");
 for(const auto& [name,path]:std::initializer_list<std::pair<const char*,const char*>>{
  {"Hat","Head/Hat"},{"Sunglasses","Head/Glasses"},
  {"Upper-clothes","Body/Clothing/Upper clothing"},
  {"Dress","Body/Clothing/Dress"},{"Skirt","Body/Lower body/Clothing/Skirt"},
  {"Pants","Body/Lower body/Clothing/Pants"},{"Pants > Left Leg","Body/Lower body/Clothing/Pants/Left trouser leg"},
  {"Pants > Right Leg","Body/Lower body/Clothing/Pants/Right trouser leg"},
  {"Belt","Body/Lower body/Waist/Clothing/Belt"},{"Scarf","Body/Upper body/Clothing/Scarf"},
  {"Left-arm","Body/Upper body/Left arm"},{"Right-arm","Body/Upper body/Right arm"},
  {"Left-leg","Body/Lower body/Legs/Left leg/Exposed skin"},{"Right-leg","Body/Lower body/Legs/Right leg/Exposed skin"},
  {"Left-shoe","Body/Lower body/Legs/Left leg/Foot/Footwear"},{"Right-shoe","Body/Lower body/Legs/Right leg/Foot/Footwear"},
  {"Bag","Accessories/Bag"}})
  put(indexOf(name),person+"/"+path);
 // The scene model knows 'animal', but provides no mammal/reptile species class.
 put(indexOf("main_animal")>=0?indexOf("main_animal"):indexOf("Animals"),"Animals/Animal 1");
 put(indexOf("main_vehicle")>=0?indexOf("main_vehicle"):indexOf("Vehicles"),"Vehicles/Vehicle 1");
 put(indexOf("Sky"),"Environment/Sky");
 put(indexOf("Scenery"),"Environment/Scenery");
 int personOrdinal=1,animalOrdinal=1,vehicleOrdinal=1;
 for(size_t i=0;i<n;++i){
  if(used[i])continue;
  const std::string name=old[i].name;
  if(name=="Clothing"||name=="Skin"||name=="Accessories"||name=="Hair"||name=="Face"||
     name=="Human > Head > Face"||name=="Human > Head > Hair"||name=="People"||
     name=="Animals"||name=="Vehicles"||name.rfind("main_person",0)==0||
     name.rfind("main_animal",0)==0||name.rfind("main_vehicle",0)==0||
     name.find("_person_")!=std::string::npos||name.find("_animal_")!=std::string::npos||
     name.find("_vehicle_")!=std::string::npos)continue;
  if(name.rfind("person_",0)==0 && (name.find("_clothing")!=std::string::npos||
     name.find("_skin")!=std::string::npos||name.find("_accessories")!=std::string::npos))continue;
  if(name.rfind("person_",0)==0){put(int(i),"People/Human/Person "+std::to_string(++personOrdinal));continue;}
  if(name.rfind("animal_",0)==0){put(int(i),"Animals/Animal "+std::to_string(++animalOrdinal));continue;}
  if(name.rfind("vehicle_",0)==0){put(int(i),"Vehicles/Vehicle "+std::to_string(++vehicleOrdinal));continue;}
  if(name.rfind("tree_",0)==0){put(int(i),"Environment/Trees/"+name);continue;}
  // Unknown specialist masks remain reachable under their model category.
  if(name.rfind("Human >",0)==0)continue;
  put(int(i),"Other/"+name);
 }
}
static std::string formatRgb(uint64_t r,uint64_t g,uint64_t b,uint64_t n){
 if(!n)return "unavailable";
 return "RGB "+std::to_string((r+n/2)/n)+", "+std::to_string((g+n/2)/n)+", "+std::to_string((b+n/2)/n);
}
static void describeImage(const ImageRGBA& image,const Mask& skin,const Mask& sky,AnalysisResult& result){
 std::array<uint64_t,256> histogram{};
 uint64_t count=0,sumR=0,sumG=0,sumB=0,skinCount=0,skinR=0,skinG=0,skinB=0,skyCount=0;
 for(size_t i=0;i<skin.pixels.size();++i){const uint8_t* p=&image.pixels[i*4];
  if(p[3]<128)continue;
  ++count;sumR+=p[0];sumG+=p[1];sumB+=p[2];
  const int luma=(54*int(p[0])+183*int(p[1])+19*int(p[2])+128)>>8;
  ++histogram[std::clamp(luma,0,255)];
  if(sky.pixels[i])++skyCount;
  if(skin.pixels[i]){++skinCount;skinR+=p[0];skinG+=p[1];skinB+=p[2];}
 }
 if(!count){result.imageReport="No visible source pixels for image colour measurements.";return;}
 auto percentile=[&](uint64_t numerator){uint64_t goal=(count*numerator+99)/100,seen=0;
  for(int i=0;i<256;++i){seen+=histogram[i];if(seen>=goal)return i;}return 255;
 };
 const int p10=percentile(10),p50=percentile(50),p90=percentile(90);
 std::string brightness=p50<68?"dark":p50>190?"bright":"moderate";
 const int warmth=int(sumR/count)-int(sumB/count);
 std::string cast=warmth>18?"warm":warmth< -18?"cool":"neutral/mixed";
 const std::string rgb=formatRgb(sumR,sumG,sumB,count);
 const std::string skinRgb=formatRgb(skinR,skinG,skinB,skinCount);
 result.imageReport="Visible-pixel image measurements (sRGB; original image):\n"
  "  Brightness: "+brightness+"; luminance percentiles 10/50/90: "+
  std::to_string(p10)+"/"+std::to_string(p50)+"/"+std::to_string(p90)+" of 255\n"+
  "  Mean image colour: "+rgb+"; overall colour cast: "+cast+"\n"+
  "  Detected visible skin pixels: "+std::to_string(skinCount)+"; observed colour: "+skinRgb+"\n"+
  "  Model-labelled sky pixels: "+std::to_string(skyCount)+
  "; daylight cue: "+(skyCount>count/50?"visible sky":"no clear sky")+"\n"+
  "  Dark-region luminance (ambient-fill proxy): "+std::to_string(p10)+" of 255\n"+
  "  Daylight vs ambient/artificial light: undetermined from this image.\n"
  "  These are image pixel measurements, not intrinsic skin pigmentation or physical light-source estimates.\n";
 result.descriptiveNodes={"Scene/Lighting/Observed brightness: "+brightness,
  "Scene/Lighting/Colour cast: "+cast,
  "Scene/Lighting/Luminance P10-P50-P90: "+std::to_string(p10)+"-"+
     std::to_string(p50)+"-"+std::to_string(p90),
  "Scene/Lighting/Sky cue: "+std::string(skyCount>count/50?"visible sky":"no clear sky"),
  "Scene/Lighting/Dark-region brightness proxy: "+std::to_string(p10)+" of 255",
  "Scene/Lighting/Daylight vs ambient source: undetermined"};
 if(skinCount)result.descriptiveNodes.push_back(
  "People/Human/Person 1/Body/Skin colour/Observed pixel colour: "+skinRgb);
}
// Pixel-evidence skin atlas. Unlike geometry priors, these masks require an observed
// local colour/texture feature inside the semantic skin mask.
static size_t addSkinSurfaceAtlas(AnalysisResult& r,const ImageRGBA& im,const Mask& skin){
 if(skin.empty()||maskArea(skin)<96)return 0;
 const int W=im.width,H=im.height;size_t n=0;
 double mr=0,mg=0,mb=0,ml=0;size_t count=0;
 for(size_t i=0;i<skin.pixels.size();++i)if(skin.pixels[i]&&im.pixels[i*4+3]){
  const auto*q=&im.pixels[i*4];mr+=q[0];mg+=q[1];mb+=q[2];ml+=(3*q[0]+6*q[1]+q[2])/10.0;++count;
 }
 if(!count)return 0;mr/=count;mg/=count;mb/=count;ml/=count;
 auto blank=[&](){return Mask{W,H,std::vector<uint8_t>(size_t(W)*H)};};
 Mask pigment=blank(),darkSpot=blank(),redBlue=blank(),crease=blank(),hair=blank();
 auto lum=[&](int x,int y){const auto*q=&im.pixels[(size_t(y)*W+x)*4];return (3*q[0]+6*q[1]+q[2])/10;};
 for(int y=1;y<H-1;++y)for(int x=1;x<W-1;++x){size_t i=size_t(y)*W+x;if(!skin.pixels[i])continue;const auto*q=&im.pixels[i*4];
  const double d=std::abs(q[0]-mr)+std::abs(q[1]-mg)+std::abs(q[2]-mb);
  if(d>42) pigment.pixels[i]=255;
  if(lum(x,y)<ml*.62 && d>36) darkSpot.pixels[i]=255;
  // Red/blue vessel-like colour cue; deliberately named as an appearance cue, not a medical vein diagnosis.
  if((int(q[0])-int(q[1])>22)||(int(q[2])-int(q[1])>20))redBlue.pixels[i]=255;
  const int gx=std::abs(lum(x+1,y)-lum(x-1,y)),gy=std::abs(lum(x,y+1)-lum(x,y-1));
  if(gx+gy>46)crease.pixels[i]=255;
  if(lum(x,y)<ml*.48 && (gx+gy)>58)hair.pixels[i]=255;
 }
 auto publish=[&](const char*leaf,const Mask&m,float cf){
  if(maskArea(m)<24)return;std::string name="People/Human/Person 1/Skin surface/";name+=leaf;
  NamedMask nm;nm.name=name;nm.parent="People/Human/Person 1/Skin surface";nm.category="Skin surface evidence";
  nm.mask=m;nm.source="observed skin pixel colour/texture cue";nm.confidence=cf;nm.provenance=MaskProvenance::LandmarkDerived;
  // Surface-evidence masks are already full-resolution; avoid duplicating each one into
  // component-sized full-frame bitmaps during analysis.
  r.masks.push_back(std::move(nm));++n;
 };
 publish("Pigmentation and colour-gradient changes",pigment,.28f);
 publish("Dark spot or mark candidates",darkSpot,.18f);
 publish("Visible red-blue vessel-like colour cues",redBlue,.16f);
 publish("Crease fold wrinkle edge cues",crease,.20f);
 publish("Hair follicle or strand-like dark edge cues",hair,.14f);
 // Preserve the surface atlas as evidence-only. Specialist future classifiers may
 // subdivide these candidates into scars, moles, bruising, scabs or vessels, but
 // geometry alone must never assign those medical/biological identities.
 return n;
}
// A lightweight deformable frame estimated from the silhouette itself. It gives the
// atlas a subject-relative centreline and row-wise width, so regions follow lean/pose
// instead of being clipped only by a fixed bounding box.
struct DeformFrame{int y0=0,y1=-1;std::vector<double> cx,half;bool valid=false;};
static DeformFrame estimateDeformFrame(const Mask&s){
 DeformFrame f;int x0,y0,x1,y1;if(!bounds(s,x0,y0,x1,y1))return f;f.y0=y0;f.y1=y1;
 f.cx.assign(size_t(y1-y0+1),(x0+x1)*.5);f.half.assign(f.cx.size(),std::max(1,(x1-x0+1)/2));
 for(int y=y0;y<=y1;++y){long sx=0,n=0;int lo=s.width,hi=-1;
  for(int x=x0;x<=x1;++x)if(s.pixels[size_t(y)*s.width+x]){sx+=x;++n;lo=std::min(lo,x);hi=std::max(hi,x);}
  if(n){size_t k=size_t(y-y0);f.cx[k]=double(sx)/n;f.half[k]=std::max(2.0,(hi-lo+1)*.5);}
 }
 // Smooth abrupt segmentation noise while retaining pose lean.
 for(int pass=0;pass<2;++pass){auto cx=f.cx,hw=f.half;for(size_t k=1;k+1<f.cx.size();++k){
   f.cx[k]=(cx[k-1]+2*cx[k]+cx[k+1])/4;f.half[k]=(hw[k-1]+2*hw[k]+hw[k+1])/4;
 }}
 f.valid=true;return f;
}
static Mask deformAtlasBand(const Mask&s,const DeformFrame&f,double xa,double xb,double ya,double yb){
 Mask out{s.width,s.height,std::vector<uint8_t>(s.pixels.size())};if(!f.valid)return out;
 const double h=std::max(1,f.y1-f.y0+1);
 for(int y=f.y0;y<=f.y1;++y){double v=(y-f.y0+.5)/h;if(v<ya||v>yb)continue;size_t k=size_t(y-f.y0);
  for(int x=0;x<s.width;++x){size_t i=size_t(y)*s.width+x;if(!s.pixels[i])continue;
   double u=.5+(x-f.cx[k])/(2*std::max(1.0,f.half[k]));if(u>=xa&&u<=xb)out.pixels[i]=255;
  }}return out;
}

static Mask atlasTube(const Mask&s,double ax,double ay,double bx,double by,double radius){
 Mask out{s.width,s.height,std::vector<uint8_t>(s.pixels.size())};int x0,y0,x1,y1;if(!bounds(s,x0,y0,x1,y1))return out;
 const double W=std::max(1,x1-x0+1),H=std::max(1,y1-y0+1),A=x0+ax*W,B=y0+ay*H,C=x0+bx*W,D=y0+by*H;
 const double vx=C-A,vy=D-B,ll=std::max(1.0,vx*vx+vy*vy),rr=radius*std::min(W,H);
 for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){size_t i=size_t(y)*s.width+x;if(!s.pixels[i])continue;
  double t=((x-A)*vx+(y-B)*vy)/ll;t=std::clamp(t,0.0,1.0);double dx=x-(A+t*vx),dy=y-(B+t*vy);
  if(dx*dx+dy*dy<=rr*rr)out.pixels[i]=255;
 }return out;
}
// Geometry-only atlas priors. These never invent pixels outside a detected subject;
// they partition an existing semantic mask into low-confidence, pose-normalised guide regions.
static Mask atlasBand(const Mask& subject,double xa,double xb,double ya,double yb){
 Mask out{subject.width,subject.height,std::vector<uint8_t>(subject.pixels.size())};
 int x0,y0,x1,y1;if(!bounds(subject,x0,y0,x1,y1))return out;
 const double w=std::max(1,x1-x0+1),h=std::max(1,y1-y0+1);
 for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){size_t i=size_t(y)*subject.width+x;if(!subject.pixels[i])continue;
  const double u=(x-x0+.5)/w,v=(y-y0+.5)/h;if(u>=xa&&u<=xb&&v>=ya&&v<=yb)out.pixels[i]=255;
 }return out;
}
static size_t addAtlasPriors(AnalysisResult& r,const Mask& people,const Mask& animals,const Mask& vehicles,const Mask& trees){
 size_t n=0;
 auto publish=[&](const std::string& name,const std::string& parent,const Mask& m,float confidence){
  if(maskArea(m)<24)return;
  if(std::any_of(r.masks.begin(),r.masks.end(),[&](const NamedMask& x){return x.name==name;}))return;
  NamedMask nm;nm.name=name;nm.parent=parent;nm.category="Atlas prior";nm.mask=m;
  nm.source="normalised detected-subject atlas prior";nm.confidence=confidence;nm.provenance=MaskProvenance::LandmarkDerived;
  // Atlas priors can number in the dozens. componentsOf() materialises another full-resolution
  // bitmap for every connected component, multiplying memory use and causing std::bad_alloc on
  // ordinary high-resolution photographs. The atlas mask itself is already subject-clipped;
  // defer component expansion until a component is explicitly requested/exported.
  r.masks.push_back(std::move(nm));++n;
 };
 auto first=[&](const Mask& combined){auto v=significantInstances(combined);return v.empty()?Mask{combined.width,combined.height,std::vector<uint8_t>(combined.pixels.size())}:std::move(v.front());};
 std::function<void(const std::string&,const std::string&,const Mask&,double,double,double,double,float)> band=
  [&](const std::string& name,const std::string& parent,const Mask&s,double xa,double xb,double ya,double yb,float cf){publish(name,parent,atlasBand(s,xa,xb,ya,yb),cf);};
 auto human=first(people); if(maskArea(human)){
  const std::string p="People/Human/Person 1/Atlas";
  const DeformFrame humanFrame=estimateDeformFrame(human);
  // Replace the fixed horizontal coordinate with the silhouette centreline. Existing
  // atlas definitions below now bend/lean with the detected person.
  band=[&](const std::string& name,const std::string& parent,const Mask&s,double xa,double xb,double ya,double yb,float cf){
   publish(name,parent,(&s==&human&&humanFrame.valid)?deformAtlasBand(s,humanFrame,xa,xb,ya,yb):atlasBand(s,xa,xb,ya,yb),cf);
  };
  // Whole-body reference: regions overlap intentionally at joints because this is a prior, not ground truth.
  band(p+"/Head/Scalp",p+"/Head",human,.31,.69,.00,.075,.22f);
  band(p+"/Head/Face",p+"/Head",human,.31,.69,.045,.165,.26f);
  band(p+"/Head/Ears",p+"/Head",human,.25,.75,.065,.145,.16f);
  band(p+"/Head/Face/Forehead",p+"/Head/Face",human,.36,.64,.055,.090,.18f);
  band(p+"/Head/Face/Eyebrows",p+"/Head/Face",human,.34,.66,.078,.102,.18f);
  band(p+"/Head/Face/Eyes and eyelids",p+"/Head/Face",human,.34,.66,.095,.120,.20f);
  band(p+"/Head/Face/Nasal root and bridge",p+"/Head/Face/Nose",human,.455,.545,.092,.140,.22f);
  band(p+"/Head/Face/Nose tip and nostrils",p+"/Head/Face/Nose",human,.425,.575,.132,.158,.20f);
  band(p+"/Head/Face/Cheeks and cheekbones",p+"/Head/Face",human,.34,.66,.112,.155,.15f);
  band(p+"/Head/Face/Mouth and lips",p+"/Head/Face",human,.405,.595,.150,.174,.20f);
  band(p+"/Head/Face/Chin and jaw",p+"/Head/Face",human,.35,.65,.165,.195,.17f);
  band(p+"/Neck",p,human,.42,.58,.155,.225,.24f);
  band(p+"/Upper body/Shoulders",p+"/Upper body",human,.18,.82,.19,.285,.20f);
  band(p+"/Upper body/Chest and ribs",p+"/Upper body",human,.30,.70,.22,.39,.21f);
  band(p+"/Torso/Abdomen",p+"/Torso",human,.32,.68,.36,.49,.20f);
  band(p+"/Torso/Waist and pelvis",p+"/Torso",human,.29,.71,.47,.59,.20f);
  band(p+"/Upper body/Right arm/Upper arm",p+"/Upper body/Right arm",human,.10,.34,.235,.42,.17f);
  band(p+"/Upper body/Right arm/Elbow",p+"/Upper body/Right arm",human,.08,.32,.38,.47,.16f);
  band(p+"/Upper body/Right arm/Forearm",p+"/Upper body/Right arm",human,.06,.31,.43,.58,.16f);
  band(p+"/Upper body/Right arm/Wrist and hand",p+"/Upper body/Right arm",human,.03,.31,.55,.69,.15f);
  band(p+"/Upper body/Left arm/Upper arm",p+"/Upper body/Left arm",human,.66,.90,.235,.42,.17f);
  band(p+"/Upper body/Left arm/Elbow",p+"/Upper body/Left arm",human,.68,.92,.38,.47,.16f);
  band(p+"/Upper body/Left arm/Forearm",p+"/Upper body/Left arm",human,.69,.94,.43,.58,.16f);
  band(p+"/Upper body/Left arm/Wrist and hand",p+"/Upper body/Left arm",human,.69,.97,.55,.69,.15f);
  band(p+"/Lower body/Right leg/Thigh",p+"/Lower body/Right leg",human,.24,.51,.55,.73,.19f);
  band(p+"/Lower body/Right leg/Knee",p+"/Lower body/Right leg",human,.23,.51,.70,.78,.18f);
  band(p+"/Lower body/Right leg/Shin and calf",p+"/Lower body/Right leg",human,.22,.51,.76,.93,.18f);
  band(p+"/Lower body/Right leg/Ankle and foot",p+"/Lower body/Right leg",human,.18,.52,.90,1.0,.17f);
  band(p+"/Lower body/Left leg/Thigh",p+"/Lower body/Left leg",human,.49,.76,.55,.73,.19f);
  band(p+"/Lower body/Left leg/Knee",p+"/Lower body/Left leg",human,.49,.77,.70,.78,.18f);
  band(p+"/Lower body/Left leg/Shin and calf",p+"/Lower body/Left leg",human,.49,.78,.76,.93,.18f);
  band(p+"/Lower body/Left leg/Ankle and foot",p+"/Lower body/Left leg",human,.48,.82,.90,1.0,.17f);
  // Fine hand atlas. These are intentionally weak priors until a dedicated hand landmark model is available.
  band(p+"/Upper body/Right arm/Hand/Palm",p+"/Upper body/Right arm/Hand",human,.08,.27,.575,.645,.12f);
  band(p+"/Upper body/Right arm/Hand/Knuckles",p+"/Upper body/Right arm/Hand",human,.045,.26,.605,.645,.10f);
  band(p+"/Upper body/Right arm/Hand/Fingers",p+"/Upper body/Right arm/Hand",human,.025,.22,.625,.690,.10f);
  band(p+"/Upper body/Right arm/Hand/Fingernails",p+"/Upper body/Right arm/Hand/Fingers",human,.020,.18,.665,.700,.07f);
  band(p+"/Upper body/Left arm/Hand/Palm",p+"/Upper body/Left arm/Hand",human,.73,.92,.575,.645,.12f);
  band(p+"/Upper body/Left arm/Hand/Knuckles",p+"/Upper body/Left arm/Hand",human,.74,.955,.605,.645,.10f);
  band(p+"/Upper body/Left arm/Hand/Fingers",p+"/Upper body/Left arm/Hand",human,.78,.975,.625,.690,.10f);
  band(p+"/Upper body/Left arm/Hand/Fingernails",p+"/Upper body/Left arm/Hand/Fingers",human,.82,.980,.665,.700,.07f);
  // Articulated coarse limb tubes: these overlap the silhouette bands and provide a
  // joint-oriented prior that can later be replaced/refined by specialist keypoints.
  publish(p+"/Skeleton/Right upper arm",p+"/Skeleton",atlasTube(human,.31,.25,.20,.39,.055),.13f);
  publish(p+"/Skeleton/Right forearm",p+"/Skeleton",atlasTube(human,.20,.39,.13,.56,.045),.13f);
  publish(p+"/Skeleton/Left upper arm",p+"/Skeleton",atlasTube(human,.69,.25,.80,.39,.055),.13f);
  publish(p+"/Skeleton/Left forearm",p+"/Skeleton",atlasTube(human,.80,.39,.87,.56,.045),.13f);
  publish(p+"/Skeleton/Right thigh",p+"/Skeleton",atlasTube(human,.42,.57,.38,.73,.070),.14f);
  publish(p+"/Skeleton/Right lower leg",p+"/Skeleton",atlasTube(human,.38,.73,.36,.92,.055),.14f);
  publish(p+"/Skeleton/Left thigh",p+"/Skeleton",atlasTube(human,.58,.57,.62,.73,.070),.14f);
  publish(p+"/Skeleton/Left lower leg",p+"/Skeleton",atlasTube(human,.62,.73,.64,.92,.055),.14f);
  // Five separate digit rays per hand. Low confidence: image/landmark evidence must
  // outrank these when a hand model is installed.
  const char* digit[5]={"Thumb","Index finger","Middle finger","Ring finger","Little finger"};
  for(int d=0;d<5;++d){double q=d/4.0;
   publish(p+"/Upper body/Right arm/Hand/"+digit[d],p+"/Upper body/Right arm/Hand",
    atlasTube(human,.20-.055*q,.61+.012*q,.08-.045*q,.67+.018*q,.018),.06f);
   publish(p+"/Upper body/Left arm/Hand/"+digit[d],p+"/Upper body/Left arm/Hand",
    atlasTube(human,.80+.055*q,.61+.012*q,.92+.045*q,.67+.018*q,.018),.06f);
  }
 }
 auto animal=first(animals); if(maskArea(animal)){
  const std::string a="Animals/Animal 1/Atlas";
  const DeformFrame animalFrame=estimateDeformFrame(animal);
  band=[&](const std::string& name,const std::string& parent,const Mask&s,double xa,double xb,double ya,double yb,float cf){
   publish(name,parent,(&s==&animal&&animalFrame.valid)?deformAtlasBand(s,animalFrame,xa,xb,ya,yb):atlasBand(s,xa,xb,ya,yb),cf);
  };
  band(a+"/Head/Skull and face",a+"/Head",animal,.00,.34,.08,.48,.17f);
  band(a+"/Head/Ears horns or antlers",a+"/Head",animal,.00,.36,.00,.25,.12f);
  band(a+"/Head/Eyes",a+"/Head",animal,.06,.29,.14,.29,.13f);
  band(a+"/Head/Muzzle snout nose mouth",a+"/Head",animal,.00,.30,.27,.52,.14f);
  band(a+"/Neck",a,animal,.23,.43,.18,.55,.15f);
  band(a+"/Body/Chest shoulders torso abdomen",a+"/Body",animal,.30,.78,.20,.68,.17f);
  band(a+"/Body/Back spine",a+"/Body",animal,.30,.82,.13,.37,.13f);
  band(a+"/Body/Front limbs",a+"/Body",animal,.22,.50,.50,1.0,.14f);
  band(a+"/Body/Hind limbs",a+"/Body",animal,.58,.88,.50,1.0,.14f);
  band(a+"/Body/Paws hooves claws",a+"/Body",animal,.15,.90,.82,1.0,.11f);
  band(a+"/Body/Tail",a+"/Body",animal,.76,1.0,.12,.72,.11f);
  band(a+"/Body/Wings or fins",a+"/Body",animal,.22,.88,.20,.75,.09f);
  publish(a+"/Skeleton/Spine",a+"/Skeleton",atlasTube(animal,.24,.32,.80,.32,.055),.10f);
  publish(a+"/Skeleton/Front limb axis",a+"/Skeleton",atlasTube(animal,.38,.48,.34,.88,.055),.09f);
  publish(a+"/Skeleton/Hind limb axis",a+"/Skeleton",atlasTube(animal,.72,.48,.76,.88,.055),.09f);
 }
 auto vehicle=first(vehicles); if(maskArea(vehicle)){
  const std::string v="Vehicles/Vehicle 1/Atlas";
  const DeformFrame vehicleFrame=estimateDeformFrame(vehicle);
  band=[&](const std::string& name,const std::string& parent,const Mask&s,double xa,double xb,double ya,double yb,float cf){
   publish(name,parent,(&s==&vehicle&&vehicleFrame.valid)?deformAtlasBand(s,vehicleFrame,xa,xb,ya,yb):atlasBand(s,xa,xb,ya,yb),cf);
  };
  band(v+"/Body/Front section",v+"/Body",vehicle,.00,.30,.20,.82,.14f);
  band(v+"/Body/Centre chassis or fuselage",v+"/Body",vehicle,.25,.75,.20,.82,.16f);
  band(v+"/Body/Rear section",v+"/Body",vehicle,.70,1.0,.20,.82,.14f);
  band(v+"/Cabin roof windows",v,vehicle,.18,.82,.00,.48,.16f);
  band(v+"/Doors panels cargo area",v+"/Body",vehicle,.20,.85,.35,.78,.13f);
  band(v+"/Running gear wheels tyres tracks",v,vehicle,.00,1.0,.68,1.0,.15f);
  band(v+"/Lights bumpers grille",v,vehicle,.00,.25,.40,.88,.11f);
  band(v+"/Mirrors external attachments",v,vehicle,.05,.95,.12,.55,.09f);
  publish(v+"/Keypoints/Longitudinal centre",v+"/Keypoints",atlasTube(vehicle,.08,.55,.92,.55,.045),.10f);
  publish(v+"/Keypoints/Front running-gear axis",v+"/Keypoints",atlasTube(vehicle,.22,.72,.22,.93,.055),.09f);
  publish(v+"/Keypoints/Rear running-gear axis",v+"/Keypoints",atlasTube(vehicle,.78,.72,.78,.93,.055),.09f);
 }
 auto tree=first(trees); if(maskArea(tree)){
  const std::string t="Scenery/Trees/Tree 1/Atlas";
  const DeformFrame treeFrame=estimateDeformFrame(tree);
  band=[&](const std::string& name,const std::string& parent,const Mask&s,double xa,double xb,double ya,double yb,float cf){
   publish(name,parent,(&s==&tree&&treeFrame.valid)?deformAtlasBand(s,treeFrame,xa,xb,ya,yb):atlasBand(s,xa,xb,ya,yb),cf);
  };
  band(t+"/Crown/Upper canopy",t+"/Crown",tree,.00,1.0,.00,.36,.16f);
  band(t+"/Crown/Middle canopy branches leaves needles",t+"/Crown",tree,.00,1.0,.25,.62,.16f);
  band(t+"/Crown/Lower branches foliage",t+"/Crown",tree,.04,.96,.50,.76,.14f);
  band(t+"/Trunk/Upper trunk",t+"/Trunk",tree,.36,.64,.42,.72,.16f);
  band(t+"/Trunk/Main trunk bark",t+"/Trunk",tree,.39,.61,.60,.94,.18f);
  band(t+"/Root flare and visible roots",t,tree,.25,.75,.88,1.0,.13f);
  publish(t+"/Structure/Trunk axis",t+"/Structure",atlasTube(tree,.50,.42,.50,.96,.055),.13f);
  publish(t+"/Structure/Left primary branch",t+"/Structure",atlasTube(tree,.50,.54,.22,.30,.035),.08f);
  publish(t+"/Structure/Right primary branch",t+"/Structure",atlasTube(tree,.50,.54,.78,.30,.035),.08f);
 }
 return n;
}
static void addAnatomyCatalog(AnalysisResult& result){
 const std::string p="People/Human/Person 1";
 bool human=false,animal=false;
 for(const auto& mask:result.masks){
  if(mask.name==p)human=true;
  if(mask.name=="Animals/Animal 1")animal=true;
 }
 auto add=[&](const std::string& path){
  if(std::none_of(result.masks.begin(),result.masks.end(),[&](const NamedMask& m){return m.name==path;}))
   result.unavailableParts.push_back(path);
 };
 if(human){
  for(const char* path:{
   "/Head/Scalp","/Head/Face/Eyelids/Left eyelid","/Head/Face/Eyelids/Right eyelid",
   "/Head/Face/Eyelashes/Left eyelash","/Head/Face/Eyelashes/Right eyelash",
   "/Head/Face/Cheeks/Left cheek","/Head/Face/Cheeks/Right cheek",
   "/Head/Face/Cheekbones/Left cheekbone","/Head/Face/Cheekbones/Right cheekbone",
   "/Head/Face/Forehead","/Head/Face/Chin","/Head/Face/Jaw",
   "/Head/Face/Nose/Nostrils/Left nostril","/Head/Face/Nose/Nostrils/Right nostril",
   "/Head/Face/Mouth/Teeth","/Head/Face/Mouth/Tongue",
   "/Head/Face/Facial hair/Beard","/Head/Face/Facial hair/Moustache",
   "/Body/Upper body/Chest/Ribs","/Body/Upper body/Chest/Chest hair",
   "/Body/Upper body/Shoulders","/Body/Upper body/Left arm/Elbow",
   "/Body/Upper body/Left arm/Wrist","/Body/Upper body/Left arm/Arm hair",
   "/Body/Upper body/Left arm/Hand/Fingers","/Body/Upper body/Left arm/Hand/Thumb",
   "/Body/Upper body/Left arm/Hand/Fingernails",
   "/Body/Upper body/Right arm/Elbow","/Body/Upper body/Right arm/Wrist",
   "/Body/Upper body/Right arm/Arm hair",
   "/Body/Upper body/Right arm/Hand/Fingers","/Body/Upper body/Right arm/Hand/Thumb",
   "/Body/Upper body/Right arm/Hand/Fingernails",
   "/Body/Torso/Stomach and abdomen","/Body/Torso/Navel","/Body/Torso/Back",
   "/Body/Torso/Back/Spine","/Body/Torso/Back/Shoulder blades",
   "/Body/Lower body/Hips","/Body/Lower body/Waist/Pelvis",
   "/Body/Lower body/Buttocks",
   "/Body/Lower body/Legs/Left leg/Knee","/Body/Lower body/Legs/Left leg/Ankle",
   "/Body/Lower body/Legs/Left leg/Leg hair",
   "/Body/Lower body/Legs/Left leg/Foot/Heel",
   "/Body/Lower body/Legs/Left leg/Foot/Sole",
   "/Body/Lower body/Legs/Left leg/Foot/Toes",
   "/Body/Lower body/Legs/Left leg/Foot/Toenails",
   "/Body/Lower body/Legs/Right leg/Knee","/Body/Lower body/Legs/Right leg/Ankle",
   "/Body/Lower body/Legs/Right leg/Leg hair",
   "/Body/Lower body/Legs/Right leg/Foot/Heel",
   "/Body/Lower body/Legs/Right leg/Foot/Sole",
   "/Body/Lower body/Legs/Right leg/Foot/Toes",
   "/Body/Lower body/Legs/Right leg/Foot/Toenails"})add(p+path);
 }
 if(animal){const std::string a="Animals/Animal 1";
  for(const char* path:{"/Head","/Head/Muzzle or snout","/Head/Ears","/Head/Eyes",
   "/Head/Whiskers","/Head/Horns or antlers","/Head/Mouth","/Head/Mouth/Teeth",
   "/Head/Mouth/Tongue","/Body","/Body/Fur or scales","/Body/Tail",
   "/Body/Wings","/Body/Fins","/Body/Front limbs","/Body/Front limbs/Paws",
   "/Body/Front limbs/Paws/Pads","/Body/Front limbs/Paws/Claws",
   "/Body/Front limbs/Hooves","/Body/Hind limbs","/Body/Hind limbs/Paws",
   "/Body/Hind limbs/Paws/Pads","/Body/Hind limbs/Paws/Claws",
   "/Body/Hind limbs/Hooves"})add(a+path);
 }
}
// Publish conservative, low-confidence regions from already detected parent
// masks. These are image-pixel locations, not hidden-anatomy predictions.
// Apply the same local colour and shade cues to every anchored visible mask.
// The raw ONNX view stays untouched, and sibling masks remain separate.
static std::pair<size_t,size_t> refineAllVisibleBoundaries(AnalysisResult& result,const ImageRGBA& image){
 size_t masks=0,pixels=0;
 auto raw=[&](const std::string& name)->const Mask*{
  for(const auto& item:result.rawMasks)if(item.name==name)return &item.mask;
  return nullptr;
 };
 for(auto& item:result.masks){
  if(item.mask.empty()||item.mask.width!=image.width||item.mask.height!=image.height)continue;
  // Find the nearest available ancestor. Parts with no detected ancestor may
  // refine their immediate edge, but cannot grow by multiple pixels.
  std::string ancestor=item.parent;
  const Mask* parent=nullptr;
  while(!ancestor.empty()&&!parent){
   parent=raw(ancestor);
   if(!parent){auto slash=ancestor.rfind('/');ancestor=slash==std::string::npos?"":ancestor.substr(0,slash);}
  }
  std::vector<const Mask*> competitors;
  for(const auto& other:result.rawMasks){
   if(other.name==item.name||other.mask.empty()||other.mask.width!=image.width||
      other.mask.height!=image.height)continue;
   if(other.parent==item.parent && !item.parent.empty())competitors.push_back(&other.mask);
   else if(item.parent.empty()&&other.parent.empty())competitors.push_back(&other.mask);
  }
  const int radius=parent?std::clamp(std::min(image.width,image.height)/400,1,3):1;
  const size_t added=refineColourShadowBoundary(image,item.mask,parent,competitors,radius);
  if(added){
   item.source+="+local colour/shading boundary";
   // Old component pixel masks no longer describe the refined boundary.
   if(!item.components.empty()&&item.components.size()<=4)
    item.components=componentsOf(item.mask,item.name);
   else item.components.clear();
   pixels+=added;++masks;
  }
 }
 return {masks,pixels};
}

static void deriveVisibleSubregions(AnalysisResult& result,const ImageRGBA& image){
 const std::string root="People/Human/Person 1",head=root+"/Head",face=head+"/Face",body=root+"/Body";
 auto locate=[&](const std::vector<NamedMask>& list,const std::string& name)->const Mask*{
  for(const auto& m:list)if(m.name==name)return &m.mask;return nullptr;
 };
 auto region=[&](const std::string& parent,const std::string& name,double xa,double xb,double ya,double yb,
                 const std::vector<std::string>& exclude={}){
  if(locate(result.masks,name))return;
  const Mask* base=locate(result.masks,parent);
  if(!base)return;
  int x0,y0,x1,y1;if(!bounds(*base,x0,y0,x1,y1))return;
  const MaskFrame frame=estimateMaskFrame(*base,verticallyOrientedPart(parent,*base));
  const int l=x0,r=x1,t=y0,b=y1;
  if(xa>xb||ya>yb)return;
  std::vector<const Mask*> blocked;
  for(const auto& n:exclude)if(const Mask* m=locate(result.masks,n))blocked.push_back(m);
  auto construct=[&](const std::vector<NamedMask>& list){
   const Mask* m=locate(list,parent);Mask out{base->width,base->height,std::vector<uint8_t>(base->pixels.size())};
   if(!m)return out;
   for(int y=t;y<=b;++y)for(int x=l;x<=r;++x){size_t i=size_t(y)*m->width+x;
    if(!m->pixels[i])continue;
    const auto [u,v]=frame.coordinates(x+.5,y+.5);
    if(u<xa||u>xb||v<ya||v>yb)continue;
    if(name==face+"/Cheeks"&&u>=.36&&u<=.64)continue;
    bool blockedPixel=false;for(const Mask* other:blocked)if(other->pixels[i]){blockedPixel=true;break;}
    if(!blockedPixel)out.pixels[i]=255;
   }
   return out;
  };
  Mask refined=construct(result.masks),raw=construct(result.rawMasks);
  if(maskArea(refined)<std::max<size_t>(32,size_t(maskArea(*base)*(xb-xa)*(yb-ya))/100))return;
  auto make=[&](Mask m){NamedMask item;item.name=name;item.parent=parent;
   item.category="Visible derived region";item.source="parent-mask anatomical region (approximate)";
   item.provenance=MaskProvenance::LandmarkDerived;item.confidence=0.35f;item.mask=std::move(m);return item;};
  result.masks.push_back(make(std::move(refined)));
  result.rawMasks.push_back(make(std::move(raw)));
 };
 region(head,head+"/Scalp",.27,.75,.00,.06,{head+"/Hair",face+"/Left eyebrow",face+"/Right eyebrow"});
 const std::vector<std::string> facialFeatures={face+"/Left eye",face+"/Right eye",
  face+"/Left eyebrow",face+"/Right eyebrow",face+"/Nose",face+"/Mouth",head+"/Hair",head+"/Scalp"};
 region(face,face+"/Forehead",.19,.83,.02,.21,facialFeatures);
 region(face,face+"/Cheeks",.05,.95,.37,.70,{face+"/Nose",face+"/Mouth"});
 region(face,face+"/Chin",.24,.78,.73,.88,{root+"/Neck"});
 // Tie anatomical left/right to the already named eye masks. Without both
 // anchors the side of a profile face is ambiguous, so do not invent labels.
 const Mask* leftEye=locate(result.masks,face+"/Left eye");
 const Mask* rightEye=locate(result.masks,face+"/Right eye");
 int lx0,ly0,lx1,ly1,rx0,ry0,rx1,ry1;
 if(leftEye&&rightEye&&bounds(*leftEye,lx0,ly0,lx1,ly1)&&bounds(*rightEye,rx0,ry0,rx1,ry1)){
  const bool subjectLeftViewerLeft=(lx0+lx1)<(rx0+rx1);
  const int split=((lx0+lx1)/2+(rx0+rx1)/2)/2;
  const int gap=std::max(1,std::abs((lx0+lx1)/2-(rx0+rx1)/2)/12);
  const std::string cheeks=face+"/Cheeks",bones=face+"/Cheekbones",lids=face+"/Eyelids";
  auto makeMask=[&](const Mask* reference){return Mask{reference->width,reference->height,std::vector<uint8_t>(reference->pixels.size())};};
  auto splitCheek=[&](const std::vector<NamedMask>& source,bool left){
   const Mask* parent=locate(source,cheeks);
   Mask out=makeMask(leftEye);if(!parent||parent->width!=out.width||parent->height!=out.height)return out;
   const bool viewerLeft=left==subjectLeftViewerLeft;
   int x0,y0,x1,y1;if(!bounds(*parent,x0,y0,x1,y1))return out;
   for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
    if(viewerLeft?x<split-gap:x>split+gap){const size_t i=size_t(y)*out.width+x;out.pixels[i]=parent->pixels[i];}
   }
   return out;
  };
  auto cheekbone=[&](const Mask& cheek,const Mask* eye){
   Mask out=makeMask(&cheek);int x0,y0,x1,y1,ex0,ey0,ex1,ey1;
   if(!bounds(cheek,x0,y0,x1,y1)||!bounds(*eye,ex0,ey0,ex1,ey1))return out;
   const int band=std::max(2,(y1-y0+1)*32/100);
   const int top=std::max(y0,ey0+(ey1-ey0)/2);
   const int bottom=std::min(y1,top+band);
   for(int y=top;y<=bottom;++y)for(int x=x0;x<=x1;++x){
    const size_t i=size_t(y)*out.width+x;if(cheek.pixels[i])out.pixels[i]=255;
   }
   return out;
  };
  auto eyelid=[&](const std::vector<NamedMask>& source,bool left){
   const Mask* eye=locate(source,face+(left?"/Left eye":"/Right eye"));
   const Mask* skin=locate(source,face);
   Mask out=makeMask(leftEye);if(!eye||!skin||eye->width!=out.width||skin->width!=out.width)return out;
   int x0,y0,x1,y1;if(!bounds(*eye,x0,y0,x1,y1))return out;
   const int radius=std::clamp((y1-y0+1)/5,2,8);
   const Mask* brow=locate(source,face+(left?"/Left eyebrow":"/Right eyebrow"));
   const Mask* nose=locate(source,face+"/Nose");
   for(int y=std::max(0,y0-radius);y<=std::min(out.height-1,y1+radius);++y)
    for(int x=std::max(0,x0-radius);x<=std::min(out.width-1,x1+radius);++x){
     const size_t i=size_t(y)*out.width+x;
     if(!skin->pixels[i]||eye->pixels[i]||(brow&&brow->pixels[i])||(nose&&nose->pixels[i]))continue;
     bool adjacent=false;
     for(int dy=-radius;dy<=radius&&!adjacent;++dy)for(int dx=-radius;dx<=radius;++dx){
      const int xx=x+dx,yy=y+dy;
      if(xx>=0&&xx<out.width&&yy>=0&&yy<out.height&&dx*dx+dy*dy<=radius*radius&&eye->pixels[size_t(yy)*out.width+xx]){adjacent=true;break;}
     }
     if(adjacent)out.pixels[i]=255;
    }
   return out;
  };
  auto eyelash=[&](const std::vector<NamedMask>& source,const Mask& lid,bool left){
   Mask out=makeMask(&lid);
   const Mask* eye=locate(source,face+(left?"/Left eye":"/Right eye"));
   const Mask* skin=locate(source,face);
   if(!eye||!skin||eye->width!=out.width||skin->width!=out.width)return out;
   int x0,y0,x1,y1;if(!bounds(*eye,x0,y0,x1,y1))return out;
   const int eyeH=y1-y0+1,eyeW=x1-x0+1;
   if(eyeW<8||eyeH<4)return out;
   const int band=std::clamp(eyeH/4,2,5);
   double skinLight=0;size_t samples=0;
   for(int y=std::max(0,y0-band*4);y<=std::min(out.height-1,y1+band*2);++y)
    for(int x=std::max(0,x0-band*2);x<=std::min(out.width-1,x1+band*2);++x){
     const size_t i=size_t(y)*out.width+x;
     if(!skin->pixels[i]||eye->pixels[i]||image.pixels[i*4+3]<128)continue;
     auto p=&image.pixels[i*4];skinLight+=(p[0]*3+p[1]*6+p[2])/10.0;++samples;
    }
   if(samples<20)return out;
   skinLight/=samples;
   for(int y=std::max(0,y0-band);y<=std::min(out.height-1,y0+band);++y)
    for(int x=std::max(0,x0-1);x<=std::min(out.width-1,x1+1);++x){
     const size_t i=size_t(y)*out.width+x;
     if(!lid.pixels[i]||image.pixels[i*4+3]<128)continue;
     auto p=&image.pixels[i*4];
     const double light=(p[0]*3+p[1]*6+p[2])/10.0;
     if(light<skinLight*.60)out.pixels[i]=255;
    }
   const size_t area=maskArea(out),eyeArea=maskArea(*eye);
   if(area<4||area>std::max<size_t>(8,eyeArea/6))std::fill(out.pixels.begin(),out.pixels.end(),0);
   return out;
  };
  auto publishPair=[&](const std::string& group,const std::string& leftName,const std::string& rightName,
                       Mask leftMask,Mask rightMask,Mask rawLeft,Mask rawRight,size_t threshold,float confidence,const char* source,bool existingGroup=false){
   if(maskArea(leftMask)<threshold||maskArea(rightMask)<threshold)return;
   auto publish=[&](std::vector<NamedMask>& dst,const std::string& name,const std::string& parent,Mask m){
    NamedMask item;item.name=name;item.parent=parent;item.category="Visible derived region";
    item.source=source;item.provenance=MaskProvenance::LandmarkDerived;item.confidence=confidence;
    item.mask=std::move(m);item.components=componentsOf(item.mask,item.name);dst.push_back(std::move(item));
   };
   if(!existingGroup){publish(result.masks,group,face,mor(leftMask,rightMask));
    publish(result.rawMasks,group,face,mor(rawLeft,rawRight));}
   publish(result.masks,group+"/"+leftName,group,std::move(leftMask));
   publish(result.rawMasks,group+"/"+leftName,group,std::move(rawLeft));
   publish(result.masks,group+"/"+rightName,group,std::move(rightMask));
   publish(result.rawMasks,group+"/"+rightName,group,std::move(rawRight));
  };
  // Compute all four side masks before adding entries: appending may move
  // vectors and invalidate the eye/cheek pointers used to derive them.
  Mask cl=splitCheek(result.masks,true),cr=splitCheek(result.masks,false);
  Mask rawCl=splitCheek(result.rawMasks,true),rawCr=splitCheek(result.rawMasks,false);
  Mask bl=cheekbone(cl,leftEye),br=cheekbone(cr,rightEye);
  Mask rawBl=cheekbone(rawCl,leftEye),rawBr=cheekbone(rawCr,rightEye);
  Mask el=eyelid(result.masks,true),er=eyelid(result.masks,false);
  Mask rawEl=eyelid(result.rawMasks,true),rawEr=eyelid(result.rawMasks,false);
  Mask lashL=eyelash(result.masks,el,true),lashR=eyelash(result.masks,er,false);
  Mask rawLashL=eyelash(result.rawMasks,rawEl,true),rawLashR=eyelash(result.rawMasks,rawEr,false);
  publishPair(cheeks,"Left cheek","Right cheek",std::move(cl),std::move(cr),std::move(rawCl),std::move(rawCr),24,.32f,"existing cheek region split at eye midline (approximate)",true);
  publishPair(bones,"Left cheekbone","Right cheekbone",std::move(bl),std::move(br),std::move(rawBl),std::move(rawBr),12,.27f,"upper visible cheek region below corresponding eye (approximate)");
  publishPair(lids,"Left eyelid","Right eyelid",std::move(el),std::move(er),std::move(rawEl),std::move(rawEr),5,.26f,"face pixels directly adjacent to corresponding eye (approximate)");
  publishPair(face+"/Eyelashes","Left eyelash","Right eyelash",std::move(lashL),std::move(lashR),std::move(rawLashL),std::move(rawLashR),4,.18f,"dark upper eyelid detail near corresponding eye (approximate)");
 }
 if(const Mask* nose=locate(result.masks,face+"/Nose")){
  int x0,y0,x1,y1;if(bounds(*nose,x0,y0,x1,y1)){
   const Mask* wholeFace=locate(result.masks,face);
   double faceBrightness=0;size_t samples=0;
   if(wholeFace)for(size_t i=0;i<wholeFace->pixels.size();i+=3)if(wholeFace->pixels[i]&&image.pixels[i*4+3]>128){
    auto p=&image.pixels[i*4];faceBrightness+=(p[0]*3+p[1]*6+p[2])/10.0;++samples;
   }
   if(samples>32){
    faceBrightness/=samples;Mask nostrils{nose->width,nose->height,std::vector<uint8_t>(nose->pixels.size())};
    for(int y=y0+(y1-y0)/3;y<=y1;++y)for(int x=x0;x<=x1;++x){size_t i=size_t(y)*nose->width+x;
     if(!nose->pixels[i])continue;auto p=&image.pixels[i*4];
     if((p[0]*3+p[1]*6+p[2])/10.0<faceBrightness*.52)nostrils.pixels[i]=255;
    }
    // Keep each side as a distinct visible component. A broad nose shadow or
    // one detected dark patch must not invent two nostril masks.
    const int noseW=x1-x0+1;
    if(noseW>=12&&maskArea(nostrils)>10){
     const size_t noseArea=maskArea(*nose);
     auto bestSide=[&](bool viewerLeft){
      Mask best{nose->width,nose->height,std::vector<uint8_t>(nose->pixels.size())};
      std::vector<uint8_t> seen(nose->pixels.size());std::vector<size_t> queue;
      const int lx=viewerLeft?x0+std::max(1,noseW/12):x0+noseW*55/100;
      const int rx=viewerLeft?x0+noseW*45/100:x1-std::max(1,noseW/12);
      size_t bestArea=0;
      for(int y=y0+(y1-y0)/3;y<=y1;++y)for(int x=lx;x<=rx;++x){
       const size_t start=size_t(y)*nose->width+x;
       if(!nostrils.pixels[start]||seen[start])continue;
       queue.clear();queue.push_back(start);seen[start]=1;
       for(size_t qi=0;qi<queue.size();++qi){const size_t i=queue[qi];int px=int(i%nose->width),py=int(i/nose->width);
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
         const int xx=px+dx,yy=py+dy;if(xx<lx||xx>rx||yy<y0||yy>y1)continue;
         const size_t j=size_t(yy)*nose->width+xx;
         if(nostrils.pixels[j]&&!seen[j]){seen[j]=1;queue.push_back(j);}
        }
       }
       // Reject a tiny speck and a dark patch covering much of the nose.
       if(queue.size()<5||queue.size()>noseArea/5||queue.size()<=bestArea)continue;
       std::fill(best.pixels.begin(),best.pixels.end(),0);
       for(size_t i:queue)best.pixels[i]=255;bestArea=queue.size();
      }
      return best;
     };
     Mask sideA=bestSide(true),sideB=bestSide(false);
     const Mask* leftEye=locate(result.masks,face+"/Left eye");
     const Mask* rightEye=locate(result.masks,face+"/Right eye");
     bool subjectLeftViewerLeft=false,eyeAnchored=false;
     int ex0,ey0,ex1,ey1,rx0,ry0,rx1,ry1;
     if(leftEye&&rightEye&&bounds(*leftEye,ex0,ey0,ex1,ey1)&&bounds(*rightEye,rx0,ry0,rx1,ry1)){
      subjectLeftViewerLeft=(ex0+ex1)<(rx0+rx1);eyeAnchored=true;
     }
     if(maskArea(sideA)>=5&&maskArea(sideB)>=5){
      Mask combined=mor(sideA,sideB);
      auto publish=[&](std::string name,std::string parent,Mask m,float confidence){
       NamedMask item;item.name=std::move(name);item.parent=std::move(parent);
       item.category="Visible derived region";
       item.source=eyeAnchored?"dark nose components, side aligned with eye masks":"dark nose components, frontal side assumption";
       item.provenance=MaskProvenance::LandmarkDerived;item.confidence=confidence;
       item.mask=std::move(m);item.components=componentsOf(item.mask,item.name);
       result.masks.push_back(item);result.rawMasks.push_back(std::move(item));
      };
      const std::string group=face+"/Nose/Nostrils";
      publish(group,face+"/Nose",std::move(combined),.38f);
      publish(group+"/Left nostril",group,subjectLeftViewerLeft?std::move(sideA):std::move(sideB),eyeAnchored?.43f:.30f);
      publish(group+"/Right nostril",group,subjectLeftViewerLeft?std::move(sideB):std::move(sideA),eyeAnchored?.43f:.30f);
     }
    }
   }
  }
 }
 // Body subregions are constrained to existing body masks, never inferred
 // from pixels behind clothing or outside the detected person.
 region(body+"/Upper body",body+"/Upper body/Shoulders",.12,.88,.30,.54,
        {body+"/Upper body/Left arm",body+"/Upper body/Right arm"});
 region(body+"/Upper body",body+"/Upper body/Chest",.27,.73,.48,.73);
 region(body+"/Torso",body+"/Torso/Stomach and abdomen",.14,.86,.20,.88);
 region(body+"/Lower body",body+"/Lower body/Hips",.18,.82,.00,.23);
}
static size_t addOccludedSurfaceProjections(AnalysisResult& result,const ImageRGBA& image,const FaceView& faceView){
 const size_t pixels=size_t(image.width)*image.height;
 if(!pixels)return 0;
 // Both the displayed projection and its blank raw counterpart are full-size
 // masks. Bound extra memory on high-resolution images.
 const size_t budget=std::min<size_t>(12,std::max<size_t>(1,48000000/(2*pixels)));
 result.masks.reserve(result.masks.size()+budget);
 result.rawMasks.reserve(result.rawMasks.size()+budget);
 auto find=[&](const std::string& name)->const Mask*{
  for(const auto& m:result.masks)if(m.name==name&&m.provenance!=MaskProvenance::OcclusionProjected)return &m.mask;
  return nullptr;
 };
 struct Candidate{std::string anchor,label;ProjectionBand band=ProjectionBand::Centre;};
 std::vector<Candidate> candidates;
 const std::string person="People/Human/Person 1",head=person+"/Head";
 if(find(person)){
  const bool face=find(head+"/Face")!=nullptr;
  const bool hair=find(head+"/Hair")!=nullptr;
  const std::string view=faceView.profile?"Opposite side":
   faceView.leftEye&&faceView.rightEye?"Rear":!face&&hair?"Front":"Unseen";
  for(const auto& [path,label]:std::initializer_list<std::pair<const char*,const char*>>{
   {"/Head","Head"},{"/Body/Upper body","Upper body"},
   {"/Body/Torso","Torso"},{"/Body/Lower body","Lower body"},
   {"/Body/Upper body/Left arm","Left arm"},{"/Body/Upper body/Right arm","Right arm"},
   {"/Body/Lower body/Legs/Left leg","Left leg"},{"/Body/Lower body/Legs/Right leg","Right leg"}}){
   candidates.push_back({person+path,person+"/Inferred hidden surfaces/"+label+"/"+view+" surface (projection)"});
  }
  if(!faceView.profile){
   for(const auto& [path,label]:std::initializer_list<std::pair<const char*,const char*>>{
      {"/Head","Head"},{"/Body","Body"}}){
    candidates.push_back({person+path,person+"/Inferred hidden surfaces/"+label+
      "/Image-left side (projection)",ProjectionBand::ImageLeft});
    candidates.push_back({person+path,person+"/Inferred hidden surfaces/"+label+
      "/Image-right side (projection)",ProjectionBand::ImageRight});
   }
  }
 }
 for(const auto& m:result.masks){
  if(m.name.rfind("Animals/Animal ",0)==0&&m.name.find('/',15)==std::string::npos)
   candidates.push_back({m.name,m.name+"/Inferred hidden surfaces/Unseen opposite surface (projection)"});
  if(m.name.rfind("Vehicles/Vehicle ",0)==0&&m.name.find('/',17)==std::string::npos)
   candidates.push_back({m.name,m.name+"/Inferred hidden surfaces/Unseen opposite surface (projection)"});
 }
 std::stable_sort(candidates.begin(),candidates.end(),[&](const Candidate& a,const Candidate& b){
  auto priority=[&](const Candidate& c){
   if(c.anchor.rfind("Animals/",0)==0)return 1;
   if(c.anchor.rfind("Vehicles/",0)==0)return 4;
   if(c.band!=ProjectionBand::Centre)return 2;
   if(c.anchor.find("/Left arm")!=std::string::npos||c.anchor.find("/Right arm")!=std::string::npos||
      c.anchor.find("/Left leg")!=std::string::npos||c.anchor.find("/Right leg")!=std::string::npos)return 3;
   return 0;
  };
  return priority(a)<priority(b);
 });
 size_t added=0;
 for(const auto& entry:candidates){
  if(added>=budget)break;
  const Mask* anchor=find(entry.anchor);
  if(!anchor||anchor->width!=image.width||anchor->height!=image.height)continue;
  const Mask* subject=entry.anchor.rfind(person,0)==0?find(person):anchor;
  if(!subject)continue;
  Mask projection=projectHiddenFootprint(*anchor,*subject,
   verticallyOrientedPart(entry.anchor,*anchor),entry.band);
  for(size_t i=0;i<pixels;++i)if(image.pixels[i*4+3]<32)projection.pixels[i]=0;
  if(!any(projection))continue;
  NamedMask projected;projected.name=entry.label;
  projected.parent=entry.label.substr(0,entry.label.rfind('/'));
  projected.category="Inferred unseen surface (2D projection)";
  projected.source="unobserved surface; plausible 2D footprint inside "+entry.anchor+
   "; no observed colour, depth or hidden texture";
  projected.provenance=MaskProvenance::OcclusionProjected;
  projected.confidence=.12f;projected.mask=std::move(projection);
  NamedMask raw;raw.name=projected.name;raw.parent=projected.parent;
  raw.category="No raw detector for hidden surface";
  raw.source="raw ONNX model has no hidden-surface pixels";raw.confidence=0;
  raw.provenance=MaskProvenance::OcclusionProjected;
  raw.mask={image.width,image.height,std::vector<uint8_t>(pixels)};
  result.masks.push_back(std::move(projected));result.rawMasks.push_back(std::move(raw));++added;
 }
 return added;
}
static void measureMasks(AnalysisResult& result,int imageWidth,int imageHeight){
 struct Metric {int x=0,y=0,width=0,height=0;size_t area=0,fragments=0;bool frame=false;};
 std::vector<Metric> metrics(result.masks.size());
 std::unordered_map<std::string,size_t> byName;
 const size_t imageArea=size_t(imageWidth)*imageHeight;
 auto decimal=[](double value){std::ostringstream out;out<<std::fixed<<std::setprecision(2)<<value;return out.str();};
 auto csvQuote=[](const std::string& value){std::string out="\"";
  for(char c:value){if(c=='\"')out+='\"';out+=c;}return out+'\"';
 };
 for(size_t n=0;n<result.masks.size();++n){const auto& m=result.masks[n];
  Metric& a=metrics[n];int x0=m.mask.width,y0=m.mask.height,x1=-1,y1=-1;
  for(int y=0;y<m.mask.height;++y)for(int x=0;x<m.mask.width;++x){size_t k=size_t(y)*m.mask.width+x;
   if(!m.mask.pixels[k])continue;
   ++a.area;x0=std::min(x0,x);x1=std::max(x1,x);
   y0=std::min(y0,y);y1=std::max(y1,y);
   if(x==0||y==0||x+1==m.mask.width||y+1==m.mask.height)a.frame=true;
  }
  if(a.area){a.x=x0;a.y=y0;a.width=x1-x0+1;a.height=y1-y0+1;}
  // Original connected components remain a diagnostic, not a refined-mask count.
  a.fragments=m.components.size();byName.emplace(m.name,n);
 }
 std::ostringstream csv;
 csv<<"mask_path,parent_path,bbox_x_px,bbox_y_px,width_px,height_px,mask_pixels,image_coverage_pct,bbox_fill_pct,raw_components,nearest_parent_containment_pct,opposite_side_overlap_pct,touches_image_edge,visibility_status,source,confidence_hint_uncalibrated\n";
 size_t persons=0,animals=0,vehicles=0;
 for(size_t n=0;n<result.masks.size();++n){const auto& m=result.masks[n];const auto& a=metrics[n];
  if(m.name.rfind("People/Human/Person ",0)==0&&m.name.find('/',20)==std::string::npos)++persons;
  if(m.name.rfind("Animals/Animal ",0)==0&&m.name.find('/',15)==std::string::npos)++animals;
  if(m.name.rfind("Vehicles/Vehicle ",0)==0&&m.name.find('/',17)==std::string::npos)++vehicles;
  const double coverage=imageArea?100.0*a.area/imageArea:0;
  const double fill=a.width&&a.height?100.0*a.area/(size_t(a.width)*a.height):0;
  std::string nearest=m.parent;
  while(!nearest.empty()&&!byName.count(nearest)){
   auto pos=nearest.rfind('/');nearest=pos==std::string::npos?"":nearest.substr(0,pos);
  }
  double contained=-1;
  if(!nearest.empty()&&a.area){size_t common=0;const auto& parent=result.masks[byName.at(nearest)].mask;
   if(parent.width==m.mask.width&&parent.height==m.mask.height)
    for(size_t k=0;k<m.mask.pixels.size();++k)common+=(m.mask.pixels[k]!=0&&parent.pixels[k]!=0);
   contained=100.0*common/a.area;
  }
  const auto containment=contained<0?"":decimal(contained);
  double sideOverlap=0;
  std::string opposite=m.name;
  auto left=opposite.find("/Left "),right=opposite.find("/Right ");
  if(left!=std::string::npos)opposite.replace(left,6,"/Right ");
  else if(right!=std::string::npos)opposite.replace(right,7,"/Left ");
  if(opposite!=m.name&&byName.count(opposite)&&a.area){
   const auto& other=result.masks[byName.at(opposite)].mask;
   if(other.width==m.mask.width&&other.height==m.mask.height){size_t both=0;
    for(size_t k=0;k<m.mask.pixels.size();++k)both+=(m.mask.pixels[k]!=0&&other.pixels[k]!=0);
    sideOverlap=100.0*both/a.area;
   }
  }
  csv<<csvQuote(m.name)<<','<<csvQuote(nearest)<<','<<a.x<<','<<a.y<<','<<a.width<<','<<a.height<<','
     <<a.area<<','<<decimal(coverage)<<','<<decimal(fill)<<','
     <<(m.components.empty()&&a.area?"":std::to_string(a.fragments))<<','<<containment<<','<<decimal(sideOverlap)<<','
     <<(a.frame?"yes":"no")<<','
     <<(m.provenance==MaskProvenance::OcclusionProjected?"projected_hidden":"visible_or_derived")<<','
     <<csvQuote(m.source)<<','<<decimal(m.confidence)<<'\n';
  result.descriptiveNodes.push_back(m.name+"/Measurements/Bounding box: x "+std::to_string(a.x)+
      ", y "+std::to_string(a.y)+", "+std::to_string(a.width)+" x "+std::to_string(a.height)+" px");
  result.descriptiveNodes.push_back(m.name+"/Measurements/Mask area: "+std::to_string(a.area)+
      " px ("+decimal(coverage)+"% of image)");
  if(contained>=0)result.descriptiveNodes.push_back(m.name+
      "/Measurements/Within parent: "+containment+"%");
  if(sideOverlap>0.1)result.descriptiveNodes.push_back(m.name+
      "/Measurements/Review opposite-side overlap: "+decimal(sideOverlap)+"%");
  if(a.frame)result.descriptiveNodes.push_back(m.name+"/Measurements/Touches image boundary");
 }
 result.metricsCsv=csv.str();
 result.descriptiveNodes.push_back("Scene/Image dimensions: "+std::to_string(imageWidth)+" x "+
   std::to_string(imageHeight)+" px");
 result.descriptiveNodes.push_back("Scene/Detected instances/People: "+std::to_string(persons));
 result.descriptiveNodes.push_back("Scene/Detected instances/Animals: "+std::to_string(animals));
 result.descriptiveNodes.push_back("Scene/Detected instances/Vehicles: "+std::to_string(vehicles));
 result.descriptiveNodes.push_back("Scene/Visibility/Occlusion: hidden surface projections are 2D estimates; true depth and unseen detail undetermined");
 result.descriptiveNodes.push_back("Scene/Materials/Model-labelled clothing and skin only");
 result.descriptiveNodes.push_back("Scene/Measurements/All units are image pixels; physical scale unknown");
}
static const std::vector<std::string> face19={"Background","Skin","Nose","Eye-glass","Left-eye","Right-eye","Left-brow","Right-brow","Left-ear","Right-ear","Mouth","Upper-lip","Lower-lip","Hair","Hat","Ear-ring","Necklace","Neck","Cloth"};

static inline int cd(const uint8_t*a,const uint8_t*b){return abs(int(a[0])-int(b[0]))+abs(int(a[1])-int(b[1]))+abs(int(a[2])-int(b[2]));}
static Mask refineMask(const ImageRGBA& im,const Mask& seed,const Mask* skin,const RefinementSettings& st){
 if(!st.enabled||seed.pixels.empty()) return seed; Mask cur=seed; const int W=im.width,H=im.height;
 std::array<long long,3> sum{}; long long n=0; for(size_t i=0;i<cur.pixels.size();++i)if(cur.pixels[i]){auto*q=&im.pixels[i*4];sum[0]+=q[0];sum[1]+=q[1];sum[2]+=q[2];++n;} if(!n)return cur;
 uint8_t mean[3]={(uint8_t)(sum[0]/n),(uint8_t)(sum[1]/n),(uint8_t)(sum[2]/n)};
 int maxD=35+st.colourTolerance*4; int edgeLim=80+(100-st.boundaryPrecision)*4; int passes=std::clamp(st.radius,1,24);
 std::vector<uint8_t> next(cur.pixels.size());
 for(int pass=0;pass<passes;++pass){next=cur.pixels;bool changed=false;for(int y=1;y<H-1;++y)for(int x=1;x<W-1;++x){size_t i=size_t(y)*W+x;auto*q=&im.pixels[i*4];int neigh=0;size_t ids[4]={i-1,i+1,i-W,i+W};for(auto j:ids)neigh+=cur.pixels[j]?1:0;
   if(!cur.pixels[i]&&neigh){if(st.protectSkin&&skin&&skin->pixels[i])continue;int d=abs(int(q[0])-mean[0])+abs(int(q[1])-mean[1])+abs(int(q[2])-mean[2]);int local=999;for(auto j:ids)if(cur.pixels[j])local=std::min(local,cd(q,&im.pixels[j*4]));int materialBonus=st.materialContinuity*2; if(d<=maxD+materialBonus&&local<=edgeLim+materialBonus){next[i]=255;changed=true;}}
   else if(cur.pixels[i]&&neigh<=1&&pass<3){int d=abs(int(q[0])-mean[0])+abs(int(q[1])-mean[1])+abs(int(q[2])-mean[2]);if(d>maxD+st.materialContinuity*3){next[i]=0;changed=true;}}
 }cur.pixels.swap(next);if(!changed)break;}
 if(st.fillHoles){next=cur.pixels;for(int y=1;y<H-1;++y)for(int x=1;x<W-1;++x){size_t i=size_t(y)*W+x;if(!cur.pixels[i]){int n4=(cur.pixels[i-1]>0)+(cur.pixels[i+1]>0)+(cur.pixels[i-W]>0)+(cur.pixels[i+W]>0);if(n4>=3&&!(st.protectSkin&&skin&&skin->pixels[i]))next[i]=255;}}cur.pixels.swap(next);}
 if(st.removeIslands){std::vector<uint8_t> seen(cur.pixels.size());std::queue<size_t>q;const size_t minArea=std::max<size_t>(8,cur.pixels.size()/20000);for(size_t s=0;s<cur.pixels.size();++s)if(cur.pixels[s]&&!seen[s]){std::vector<size_t> comp;q.push(s);seen[s]=1;while(!q.empty()){size_t i=q.front();q.pop();comp.push_back(i);int x=i%W,y=i/W;size_t nb[4]={i?i-1:i,i+1<cur.pixels.size()?i+1:i,y?i-W:i,y+1<H?i+W:i};for(auto j:nb)if(cur.pixels[j]&&!seen[j]){seen[j]=1;q.push(j);}}if(comp.size()<minArea)for(auto i:comp)cur.pixels[i]=0;}}
 return cur;
}
}
struct SegmentationEngine::Impl{HMODULE dll=nullptr;const OrtApi*api=nullptr;OrtEnv*env=nullptr;OrtSession*scene=nullptr;OrtSession*clothes=nullptr;OrtSession*face=nullptr;OrtSession*detail=nullptr;OrtSession*wholeBodyPose=nullptr;OrtSession*handPose=nullptr;OrtSession*animalPose=nullptr;OrtSessionOptions*opts=nullptr;bool tryDml=true;bool requireDml=false;bool dmlAppended=false;};
SegmentationEngine::SegmentationEngine(const std::filesystem::path&d):p_(new Impl),scene_(d/L"scene_ade20k.onnx"),clothes_(d/L"clothes_human_parsing.onnx"),face_(d/L"face_parsing.onnx"),detail_(d/L"detailed_face_teeth.onnx"),wholeBodyPose_(d/L"human_wholebody_pose.onnx"),handPose_(d/L"hand_pose.onnx"),animalPose_(d/L"animal_pose_ap10k.onnx"){}
SegmentationEngine::~SegmentationEngine(){if(p_){if(p_->api){if(p_->scene)p_->api->ReleaseSession(p_->scene);if(p_->clothes)p_->api->ReleaseSession(p_->clothes);if(p_->face)p_->api->ReleaseSession(p_->face);if(p_->detail)p_->api->ReleaseSession(p_->detail);if(p_->wholeBodyPose)p_->api->ReleaseSession(p_->wholeBodyPose);if(p_->handPose)p_->api->ReleaseSession(p_->handPose);if(p_->animalPose)p_->api->ReleaseSession(p_->animalPose);if(p_->opts)p_->api->ReleaseSessionOptions(p_->opts);if(p_->env)p_->api->ReleaseEnv(p_->env);}if(p_->dll)FreeLibrary(p_->dll);delete p_;}}
void SegmentationEngine::setThreads(int n){threads_=std::max(1,n);} bool SegmentationEngine::modelsPresent()const{return std::filesystem::exists(scene_)&&std::filesystem::exists(clothes_);} 

struct PoseModelInfo{bool installed=false;bool sessionReady=false;size_t inputs=0,outputs=0;std::string diagnostic;};
static PoseModelInfo inspectPoseModel(SegmentationEngine::Impl*p,const std::filesystem::path&path,OrtSession*&sess){
 PoseModelInfo r;r.installed=std::filesystem::exists(path);if(!r.installed){r.diagnostic="not installed";return r;}
 auto a=p->api;try{
  if(!sess){ortck(a,a->CreateSession(p->env,path.c_str(),p->opts,&sess));}
  ortck(a,a->SessionGetInputCount(sess,&r.inputs));ortck(a,a->SessionGetOutputCount(sess,&r.outputs));
  r.sessionReady=r.inputs>0&&r.outputs>0;
  r.diagnostic=r.sessionReady?("ready ("+std::to_string(r.inputs)+" input, "+std::to_string(r.outputs)+" output tensor(s))"):"invalid tensor interface";
 }catch(const std::exception&e){r.diagnostic=e.what();}
 return r;
}
static std::vector<uint16_t> runModel(SegmentationEngine::Impl*p,const std::filesystem::path&path,OrtSession*&sess,const ImageRGBA&im,int threads,int expected,const Mask* crop=nullptr,bool easyPortrait=false){auto a=p->api;OrtAllocator*alloc=nullptr;char*in=nullptr,*out=nullptr;OrtMemoryInfo*mi=nullptr;OrtValue*t=nullptr,*yv=nullptr;OrtTensorTypeAndShapeInfo*ti=nullptr;
 auto cleanup=[&](){if(ti)a->ReleaseTensorTypeAndShapeInfo(ti);if(yv)a->ReleaseValue(yv);if(t)a->ReleaseValue(t);if(mi)a->ReleaseMemoryInfo(mi);if(alloc){if(in)alloc->Free(alloc,in);if(out)alloc->Free(alloc,out);}};
 try {if(!p->env)ortck(a,a->CreateEnv(ORT_LOGGING_LEVEL_WARNING,"VIMS",&p->env));if(!p->opts){ortck(a,a->CreateSessionOptions(&p->opts));a->SetIntraOpNumThreads(p->opts,threads);a->DisableCpuMemArena(p->opts);a->SetSessionGraphOptimizationLevel(p->opts,ORT_ENABLE_ALL);
 // DirectML is discovered dynamically so the same executable retains CPU compatibility.
 // A DirectML-enabled ONNX Runtime exports this provider factory function.
 using DmlAppend=OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*,int);
 if(p->tryDml){auto dml=reinterpret_cast<DmlAppend>(GetProcAddress(p->dll,"OrtSessionOptionsAppendExecutionProvider_DML"));
 if(!dml){if(p->requireDml)throw std::runtime_error("DirectML provider is unavailable");}
 else {
   a->DisableMemPattern(p->opts);a->SetSessionExecutionMode(p->opts,ORT_SEQUENTIAL);
   CrashLog::write("DirectML provider export found; appending execution provider");
   OrtStatus* st=dml(p->opts,0);if(st){ std::string msg=a->GetErrorMessage(st);CrashLog::write("DirectML provider append failed: "+msg);a->ReleaseStatus(st);if(p->requireDml)throw std::runtime_error("DirectML initialization failed: "+msg); } else {p->dmlAppended=true;CrashLog::write("DirectML provider append succeeded");}
 }}
}if(!sess){ CrashLog::write("Creating ONNX Runtime session"); ortck(a,a->CreateSession(p->env,path.c_str(),p->opts,&sess)); CrashLog::write("ONNX Runtime session created"); }
 size_t ni=0,no=0;a->SessionGetInputCount(sess,&ni);a->SessionGetOutputCount(sess,&no);if(!ni||!no)throw std::runtime_error("ONNX model has no input/output");a->GetAllocatorWithDefaultOptions(&alloc);ortck(a,a->SessionGetInputName(sess,0,alloc,&in));ortck(a,a->SessionGetOutputName(sess,0,alloc,&out));
 const int W=512,H=512;std::vector<float>x(3*W*H);const float mean[3]={.485f,.456f,.406f},sd[3]={.229f,.224f,.225f};
 int cx0=0,cy0=0,cx1=im.width-1,cy1=im.height-1;
 if(crop){int bx0,by0,bx1,by1;if(bounds(*crop,bx0,by0,bx1,by1)){
  int side=std::max(bx1-bx0+1,by1-by0+1)*13/10;
  int midx=(bx0+bx1)/2,midy=(by0+by1)/2;
  cx0=std::max(0,midx-side/2);cy0=std::max(0,midy-side/2);
  cx1=std::min(im.width-1,midx+side/2);cy1=std::min(im.height-1,midy+side/2);
 }}
 for(int y=0;y<H;y++)for(int xx=0;xx<W;xx++){
  int sx=cx0+xx*(cx1-cx0+1)/W,sy=cy0+y*(cy1-cy0+1)/H;
  auto q=&im.pixels[(size_t(sy)*im.width+sx)*4];
  float rgb[3]={float(q[0]),float(q[1]),float(q[2])};
  if(crop){
   double fx=cx0+(xx+.5)*(cx1-cx0+1)/double(W)-.5;
   double fy=cy0+(y+.5)*(cy1-cy0+1)/double(H)-.5;
   int ax=std::clamp(int(std::floor(fx)),0,im.width-1),ay=std::clamp(int(std::floor(fy)),0,im.height-1);
   int bx=std::min(im.width-1,ax+1),by=std::min(im.height-1,ay+1);
   float tx=float(std::clamp(fx-ax,0.0,1.0)),ty=float(std::clamp(fy-ay,0.0,1.0));
   auto tl=&im.pixels[(size_t(ay)*im.width+ax)*4],tr=&im.pixels[(size_t(ay)*im.width+bx)*4];
   auto bl=&im.pixels[(size_t(by)*im.width+ax)*4],br=&im.pixels[(size_t(by)*im.width+bx)*4];
   for(int c=0;c<3;c++)rgb[c]=(1-ty)*((1-tx)*tl[c]+tx*tr[c])+ty*((1-tx)*bl[c]+tx*br[c]);
  }
  for(int c=0;c<3;c++){
   if(easyPortrait){int channel=2-c;x[size_t(c)*W*H+y*W+xx]=(rgb[channel]-(c==0?143.55267f:c==1?132.96706f:126.94924f))/(c==0?60.26253f:c==1?60.32740f:59.30989f);}
   else x[size_t(c)*W*H+y*W+xx]=(rgb[c]/255.f-mean[c])/sd[c];
  }
 }
 ortck(a,a->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&mi));int64_t shape[4]={1,3,H,W};ortck(a,a->CreateTensorWithDataAsOrtValue(mi,x.data(),x.size()*sizeof(float),shape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&t));const char*ins[]={in},*outs[]={out};ortck(a,a->Run(sess,nullptr,ins,(const OrtValue*const*)&t,1,outs,1,&yv));ortck(a,a->GetTensorTypeAndShape(yv,&ti));size_t nd=0;a->GetDimensionsCount(ti,&nd);if(nd!=4)throw std::runtime_error("Expected [1,C,H,W] output");int64_t dims[4];a->GetDimensions(ti,dims,4);int C=(int)dims[1],oh=(int)dims[2],ow=(int)dims[3];if(expected>0&&C!=expected)throw std::runtime_error("ONNX class-count mismatch");float*logits=nullptr;ortck(a,a->GetTensorMutableData(yv,(void**)&logits));std::vector<uint16_t>small(size_t(ow)*oh);for(int yy=0;yy<oh;yy++)for(int xx=0;xx<ow;xx++){int best=0;float bs=logits[yy*ow+xx];for(int c=1;c<C;c++){float s=logits[size_t(c)*oh*ow+yy*ow+xx];if(s>bs){bs=s;best=c;}}small[yy*ow+xx]=best;}std::vector<uint16_t>full(size_t(im.width)*im.height);for(int yy=cy0;yy<=cy1;yy++)for(int xx=cx0;xx<=cx1;xx++)full[size_t(yy)*im.width+xx]=small[size_t((yy-cy0)*oh/(cy1-cy0+1))*ow+(xx-cx0)*ow/(cx1-cx0+1)];cleanup();return full;}catch(...){cleanup();throw;}}
AnalysisResult SegmentationEngine::analyse(const ImageRGBA&im){
 using Clock=std::chrono::steady_clock;const auto begin=Clock::now();auto elapsed=[](Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();};if(im.empty())throw std::runtime_error("No image loaded");if(!modelsPresent())throw std::runtime_error("Models missing. Run download_models.ps1.");if(!p_->dll){p_->dll=LoadLibraryW(L"onnxruntime.dll");if(!p_->dll)throw std::runtime_error("onnxruntime.dll not found");auto get=(const OrtApiBase*(ORT_API_CALL*)())GetProcAddress(p_->dll,"OrtGetApiBase");if(!get)throw std::runtime_error("OrtGetApiBase missing");const OrtApiBase* base=get();if(!base||!base->GetApi)throw std::runtime_error("ONNX Runtime API base missing");CrashLog::write(std::string("ONNX Runtime version: ")+(base->GetVersionString?base->GetVersionString():"unknown"));for(unsigned version=ORT_API_VERSION;version>=20;--version){p_->api=base->GetApi(version);if(p_->api){CrashLog::write("ONNX Runtime API selected: "+std::to_string(version));break;}}if(!p_->api)throw std::runtime_error("No compatible ONNX Runtime API version");}
 // Recreate sessions when switching devices so a prior DirectML session cannot survive CPU selection.
 if(p_->scene){p_->api->ReleaseSession(p_->scene);p_->scene=nullptr;}
 if(p_->clothes){p_->api->ReleaseSession(p_->clothes);p_->clothes=nullptr;}
 if(p_->face){p_->api->ReleaseSession(p_->face);p_->face=nullptr;}
 if(p_->detail){p_->api->ReleaseSession(p_->detail);p_->detail=nullptr;}
 if(p_->wholeBodyPose){p_->api->ReleaseSession(p_->wholeBodyPose);p_->wholeBodyPose=nullptr;}
 if(p_->handPose){p_->api->ReleaseSession(p_->handPose);p_->handPose=nullptr;}
 if(p_->animalPose){p_->api->ReleaseSession(p_->animalPose);p_->animalPose=nullptr;}
 if(p_->opts){p_->api->ReleaseSessionOptions(p_->opts);p_->opts=nullptr;}
 p_->tryDml=backend_!=Backend::CPU;p_->requireDml=backend_==Backend::DirectML;p_->dmlAppended=false;
 CrashLog::write(backend_==Backend::CPU?"Inference mode: CPU":backend_==Backend::DirectML?"Inference mode: DirectML required":"Inference mode: Auto");
 // Validate optional pose specialists through the same ORT/device configuration. Decoding is
 // enabled only after their exact tensor contracts are known; geometry remains the fallback.
 // Do not create optional pose sessions in the main analysis path yet. Some RTMPose
 // exports require a different input contract from the segmentation models, and probing
 // them here can make an otherwise valid analysis fail. Presence is reported safely;
 // tensor inspection/decoding is performed only by the dedicated pose path once implemented.
 PoseModelInfo wholePoseInfo,handPoseInfo,animalPoseInfo;
 wholePoseInfo.installed=std::filesystem::exists(wholeBodyPose_);wholePoseInfo.diagnostic=wholePoseInfo.installed?"installed; decoder pending":"not installed";
 handPoseInfo.installed=std::filesystem::exists(handPose_);handPoseInfo.diagnostic=handPoseInfo.installed?"installed; decoder pending":"not installed";
 animalPoseInfo.installed=std::filesystem::exists(animalPose_);animalPoseInfo.diagnostic=animalPoseInfo.installed?"installed; decoder pending":"not installed";
 CrashLog::write("Whole-body pose: "+wholePoseInfo.diagnostic);
 CrashLog::write("Hand pose: "+handPoseInfo.diagnostic);
 CrashLog::write("Animal pose: "+animalPoseInfo.diagnostic);
 const auto setupEnd=Clock::now();auto sl=runModel(p_,scene_,p_->scene,im,threads_,(int)ade.size());const auto sceneEnd=Clock::now();auto cl=runModel(p_,clothes_,p_->clothes,im,threads_,(int)clothes.size());const auto clothingEnd=Clock::now();auto clothingFace=classMask(cl,im.width,im.height,clothes,{"Face"});int fx0,fy0,fx1,fy1;bool haveFaceAnchor=bounds(clothingFace,fx0,fy0,fx1,fy1);gpuActive_=p_->dmlAppended;AnalysisResult r; std::vector<uint16_t> fl; bool haveFace=std::filesystem::exists(face_); if(haveFace) fl=runModel(p_,face_,p_->face,im,threads_,(int)face19.size(),haveFaceAnchor?&clothingFace:nullptr);const auto faceEnd=Clock::now();r.setupMs=elapsed(begin,setupEnd);r.sceneMs=elapsed(setupEnd,sceneEnd);r.clothingMs=elapsed(sceneEnd,clothingEnd);r.faceMs=elapsed(clothingEnd,faceEnd);r.usedDirectML=gpuActive_;auto sky=classMask(sl,im.width,im.height,ade,{"sky"});auto person=classMask(sl,im.width,im.height,ade,{"person"});auto hp=classMask(cl,im.width,im.height,clothes,{"Hat","Hair","Sunglasses","Upper-clothes","Skirt","Pants","Dress","Belt","Left-shoe","Right-shoe","Face","Left-leg","Right-leg","Left-arm","Right-arm","Bag","Scarf"});person=mor(person,hp);auto veh=classMask(sl,im.width,im.height,ade,{"car","boat","bus","truck","airplane","van","ship","minibike","bicycle"});auto animal=classMask(sl,im.width,im.height,ade,{"animal"});auto scenery=classMask(sl,im.width,im.height,ade,{"building","tree","road","grass","sidewalk","earth","mountain","plant","water","house","sea","field","sand","path","river","bridge","hill","land","waterfall","lake"});auto clothing=classMask(cl,im.width,im.height,clothes,{"Hat","Upper-clothes","Skirt","Pants","Dress","Belt","Left-shoe","Right-shoe","Scarf"});auto skin=classMask(cl,im.width,im.height,clothes,{"Face","Left-leg","Right-leg","Left-arm","Right-arm"});auto acc=classMask(cl,im.width,im.height,clothes,{"Sunglasses","Bag"});
 add(r.rawMasks,"Clothing",clothing);add(r.rawMasks,"Skin",skin);add(r.rawMasks,"Accessories",acc);add(r.rawMasks,"People",person);add(r.rawMasks,"Vehicles",veh);add(r.rawMasks,"Animals",animal);add(r.rawMasks,"Sky",sky);add(r.rawMasks,"Scenery",scenery); std::vector<std::pair<std::string,Mask>> personParts={{"clothing",clothing},{"skin",skin},{"accessories",acc}}; addInstances(r.rawMasks,"person",person,personParts);addInstances(r.rawMasks,"vehicle",veh);addInstances(r.rawMasks,"animal",animal); auto trees=classMask(sl,im.width,im.height,ade,{"tree"});addInstances(r.rawMasks,"tree",trees);auto upper=classMask(cl,im.width,im.height,clothes,{"Upper-clothes"});
 auto pants=classMask(cl,im.width,im.height,clothes,{"Pants"});
 auto scarf=classMask(cl,im.width,im.height,clothes,{"Scarf"});
 auto belt=classMask(cl,im.width,im.height,clothes,{"Belt"});
 auto shoeLeft=classMask(cl,im.width,im.height,clothes,{"Left-shoe"});
 auto shoeRight=classMask(cl,im.width,im.height,clothes,{"Right-shoe"});
 auto [cleanLeftShoe,cleanRightShoe]=separateShoes(shoeLeft,shoeRight);
 if(any(scarf))scarf=growAccessory(im,scarf,upper,skin,false);
 if(any(belt))belt=growAccessory(im,belt,person,skin,true,&pants);
 for(int i=1;i<(int)clothes.size();++i){
  auto m=classMask(cl,im.width,im.height,clothes,{clothes[i].c_str()});
  if(clothes[i]=="Scarf")m=scarf;
  if(clothes[i]=="Belt")m=belt;
  if(clothes[i]=="Left-shoe")m=cleanLeftShoe;
  if(clothes[i]=="Right-shoe")m=cleanRightShoe;
  if(haveFaceAnchor&&(clothes[i]=="Left-arm"||clothes[i]=="Right-arm")){
   const int armTop=fy1+(fy1-fy0+1)/3;
   for(int y=0;y<std::min(im.height,armTop);++y)
    std::fill_n(m.pixels.begin()+size_t(y)*im.width,im.width,0);
  }
  add(r.rawMasks,clothes[i].c_str(),m);
 }
 auto [leftTrouser,rightTrouser]=trouserLegs(pants);
 add(r.rawMasks,"Pants > Left Leg",leftTrouser,"Clothing","Pants","pants-region split",0.65f);
 add(r.rawMasks,"Pants > Right Leg",rightTrouser,"Clothing","Pants","pants-region split",0.65f);
 const FaceView faceView=haveFaceAnchor?estimateFaceView(clothingFace,fl):FaceView{};
 auto derived=haveFaceAnchor?deriveFaceParts(im,clothingFace,person,faceView):std::vector<std::pair<std::string,Mask>>{};
 auto plausible=[&](const std::string& name,const Mask& m){
  auto hint=std::find_if(derived.begin(),derived.end(),[&](const auto& item){return item.first==name;});
  if(hint==derived.end())return true;
  size_t area=maskArea(m),reference=maskArea(hint->second),overlap=0;
  if(reference<40||area<32)return false;
  for(size_t i=0;i<m.pixels.size();++i)overlap+=(m.pixels[i]&&hint->second.pixels[i]);
  const double minCoverage=name.find("Eyebrow")!=std::string::npos?.23:
    name.find(" Eye")!=std::string::npos?.38:
    name.find("Mouth")!=std::string::npos?.35:.20;
  return area>=reference*minCoverage && overlap>=area*.25;
 };
 int faceModelParts=0,detailParts=0,fallbackParts=0;
 if(haveFace){
  struct F{const char*label;const char*name;const char*parent;};
  const F fs[]={ {"Skin","Human > Head > Face","Human > Head"},{"Nose","Human > Head > Face > Nose","Human > Head > Face"},{"Left-eye","Human > Head > Face > Left Eye","Human > Head > Face"},{"Right-eye","Human > Head > Face > Right Eye","Human > Head > Face"},{"Left-brow","Human > Head > Face > Left Eyebrow","Human > Head > Face"},{"Right-brow","Human > Head > Face > Right Eyebrow","Human > Head > Face"},{"Left-ear","Human > Head > Left Ear","Human > Head"},{"Right-ear","Human > Head > Right Ear","Human > Head"},{"Mouth","Human > Head > Face > Mouth","Human > Head > Face"},{"Upper-lip","Human > Head > Face > Mouth > Upper Lip","Human > Head > Face > Mouth"},{"Lower-lip","Human > Head > Face > Mouth > Lower Lip","Human > Head > Face > Mouth"},{"Hair","Human > Head > Hair","Human > Head"},{"Neck","Human > Neck","Human"} };
  for(const auto& f:fs){auto m=classMask(fl,im.width,im.height,face19,{f.label});
   if(haveFaceAnchor){
    if(std::string(f.label)=="Neck")m=neckFromFaceColour(im,clothingFace,m,faceView);
    else if(std::string(f.label)!="Skin")m=headRegion(m,clothingFace,f.label,&faceView);
   }
   if(std::string(f.label)!="Skin"&&maskArea(m)>=32&&
      (std::string(f.label)=="Neck"||std::string(f.label)=="Hair"||plausible(f.name,m)))++faceModelParts;
   else if(std::string(f.label)!="Skin"&&std::string(f.label)!="Neck"&&std::string(f.label)!="Hair")
      std::fill(m.pixels.begin(),m.pixels.end(),0);
   add(r.rawMasks,f.name,m,"Human anatomy",f.parent,"face_parsing.onnx",0.90f);
  }
}
 bool usedDetail=false;double detailedFaceMs=0;
 if(haveFaceAnchor&&std::filesystem::exists(detail_)){
  try{
   const auto detailStart=Clock::now();
   const auto labels=runModel(p_,detail_,p_->detail,im,threads_,8,&clothingFace,true);
   auto viewerLeft=[&](uint16_t id){double sum=0,n=0;
    for(int y=std::max(0,fy0);y<=std::min(im.height-1,fy1);++y)
     for(int x=std::max(0,fx0);x<=std::min(im.width-1,fx1);++x)
      if(labels[size_t(y)*im.width+x]==id){sum+=x;++n;}
    return n>0&&sum/n<(fx0+fx1)*.5;
   };
   const bool browLeft=viewerLeft(2),eyeLeft=viewerLeft(4);
   struct Fine {uint16_t id;const char* name;const char* parent;};
   const Fine fine[]={
    {2,browLeft?"Human > Head > Face > Right Eyebrow":"Human > Head > Face > Left Eyebrow","Human > Head > Face"},
    {3,browLeft?"Human > Head > Face > Left Eyebrow":"Human > Head > Face > Right Eyebrow","Human > Head > Face"},
    {4,eyeLeft?"Human > Head > Face > Right Eye":"Human > Head > Face > Left Eye","Human > Head > Face"},
    {5,eyeLeft?"Human > Head > Face > Left Eye":"Human > Head > Face > Right Eye","Human > Head > Face"},
    {6,"Human > Head > Face > Mouth","Human > Head > Face"},
    {7,"Human > Head > Face > Mouth > Teeth","Human > Head > Face > Mouth"}};
   for(const auto& f:fine){
    Mask m{im.width,im.height,std::vector<uint8_t>(size_t(im.width)*im.height)};
    for(size_t i=0;i<labels.size();++i)if(labels[i]==f.id)m.pixels[i]=255;
    m=headRegion(m,clothingFace,f.name,&faceView);
    if(f.id>=2&&f.id<=5){
     const bool left=std::string(f.name).find("Left ")!=std::string::npos;
     const int mid=(fx0+fx1)/2,margin=(fx1-fx0+1)/50;
     for(int y=0;y<im.height;++y)for(int x=0;x<im.width;++x)
      if(left?x<mid-margin:x>mid+margin)m.pixels[size_t(y)*im.width+x]=0;
    }
    size_t area=maskArea(m);if(area<24||area>size_t((fx1-fx0+1)*(fy1-fy0+1))/5)continue;
    if(f.id==7){
     auto mouth=std::find_if(r.rawMasks.begin(),r.rawMasks.end(),[](const NamedMask& old){return old.name=="Human > Head > Face > Mouth";});
     size_t inside=0;if(mouth!=r.rawMasks.end())for(size_t i=0;i<m.pixels.size();++i)inside+=(m.pixels[i]&&mouth->mask.pixels[i]);
     if(area<80||inside*100<area*60)continue;
    }else if(!plausible(f.name,m))continue;
    auto current=std::find_if(r.rawMasks.begin(),r.rawMasks.end(),[&](const NamedMask& old){return old.name==f.name;});
    if(current!=r.rawMasks.end()){
     // Prefer the specialist already in use when it found a substantial part.
     if(maskArea(current->mask)>=area/3)continue;
     r.rawMasks.erase(current);
    }
    add(r.rawMasks,f.name,m,"Human anatomy",f.parent,"detailed_face_teeth.onnx",.84f);
    usedDetail=true;++detailParts;
   }
   detailedFaceMs+=elapsed(detailStart,Clock::now());r.faceMs+=detailedFaceMs;
  }catch(const std::exception& e){
   CrashLog::write(std::string("Optional detailed face model unavailable: ")+e.what());
   if(p_->detail){p_->api->ReleaseSession(p_->detail);p_->detail=nullptr;}
  }
 }
 if(haveFaceAnchor){
  const std::vector<std::string> priority={"Human > Head > Face > Left Eye","Human > Head > Face > Right Eye",
   "Human > Head > Face > Mouth","Human > Head > Face > Nose",
   "Human > Head > Face > Left Eyebrow","Human > Head > Face > Right Eyebrow",
   "Human > Head > Left Ear","Human > Head > Right Ear"};
  Mask occupied{im.width,im.height,std::vector<uint8_t>(size_t(im.width)*im.height)};
  for(const auto& name:priority){
   auto candidate=std::find_if(derived.begin(),derived.end(),[&](const auto& item){return item.first==name;});
   if(candidate==derived.end())continue;
   auto current=std::find_if(r.rawMasks.begin(),r.rawMasks.end(),[&](const NamedMask& item){return item.name==name;});
   const double minPart=name.find("Eyebrow")!=std::string::npos?.23:
     name.find(" Eye")!=std::string::npos?.38:
     name.find("Mouth")!=std::string::npos?.35:.20;
   if(current!=r.rawMasks.end()&&maskArea(current->mask)>=std::max<size_t>(32,size_t(maskArea(candidate->second)*minPart))){
    for(size_t i=0;i<occupied.pixels.size();++i)occupied.pixels[i]|=current->mask.pixels[i];
    continue;
   }
   for(size_t i=0;i<occupied.pixels.size();++i)if(occupied.pixels[i])candidate->second.pixels[i]=0;
   if(current!=r.rawMasks.end())r.rawMasks.erase(current);
   add(r.rawMasks,name.c_str(),candidate->second,"Human anatomy","Human > Head","face-region colour and contrast",0.48f);
   if(!r.rawMasks.empty()&&r.rawMasks.back().name==name)r.rawMasks.back().provenance=MaskProvenance::LandmarkDerived;
   if(maskArea(candidate->second)>=32)++fallbackParts;
   for(size_t i=0;i<occupied.pixels.size();++i)occupied.pixels[i]|=candidate->second.pixels[i];
  }
 }
 const auto masksEnd=Clock::now();r.masksMs=std::max(0.0,elapsed(faceEnd,masksEnd)-detailedFaceMs);consolidateLeafDuplicates(r.rawMasks); r.masks=r.rawMasks; if(settings_.enabled){const std::set<std::string> garment={"Clothing","Hat","Upper-clothes","Skirt","Pants","Dress","Left-shoe","Right-shoe"};for(auto&m:r.masks)if(garment.count(m.name))m.mask=refineMask(im,m.mask,&skin,settings_);}
 organizeHierarchy(r);
 describeImage(im,skin,sky,r);
 {
  std::ostringstream faceReport;
  faceReport<<"\nFace model diagnostics: face anchor "<<(haveFaceAnchor?"detected":"absent")
    <<", 19-class model "<<(haveFace?"installed":"absent")<<", accepted model parts "<<faceModelParts
    <<", detailed model "<<(std::filesystem::exists(detail_)?"installed":"absent")
    <<", accepted detailed parts "<<detailParts<<", derived fallback parts "<<fallbackParts
    <<", estimated image roll "<<int(faceView.roll*180.0/3.14159265358979323846)
    <<" degrees, view "<<(faceView.profile?"one visible eye/profile candidate":
      faceView.leftEye&&faceView.rightEye?"two-eye pose":"orientation uncertain")<<".";
  r.imageReport+=faceReport.str();CrashLog::write(faceReport.str());
 }
 r.imageReport+="\nPose specialists: whole-body "+wholePoseInfo.diagnostic+", hand "+handPoseInfo.diagnostic+", animal "+animalPoseInfo.diagnostic+
  ". Geometric atlas fallback remains active until each model's output tensors are decoded and confidence-filtered.";
 deriveVisibleSubregions(r,im);
 if(settings_.enabled){
  const auto [refinedParts,refinedPixels]=refineAllVisibleBoundaries(r,im);
  r.imageReport+="\nColour and shading refinement: "+std::to_string(refinedParts)+
    " detected masks, "+std::to_string(refinedPixels)+" added boundary pixels within their existing regions.";
 }
 // A rough rectangle supplies trustworthy negative pixels outside its bounds
 // for the very same decoded image. It is not a positive label or a model
 // update. The raw view remains the unmodified model output.
 try{
  const auto guides=loadTrainingGuides(scene_.parent_path().parent_path()/L"TrainingGuides",im);
  size_t affected=0,removed=0;
  for(const auto& guide:guides){
   bool matched=false;
   const double angle=guide.angleDegrees*3.14159265358979323846/180.0;
   const double c=std::cos(angle),s=std::sin(angle);
   const double cx=(guide.left+guide.right)*.5,cy=(guide.top+guide.bottom)*.5;
   const double halfW=(guide.right-guide.left)*.5,halfH=(guide.bottom-guide.top)*.5;
   for(auto& item:r.masks){
    const std::string label=trainingGuideSafeLabel(item.name);
    if(label!=guide.label&&!(label.size()>guide.label.size()&&label.compare(0,guide.label.size(),guide.label)==0&&label[guide.label.size()]=='_'))continue;
    if(item.mask.empty()||item.mask.width!=im.width||item.mask.height!=im.height)continue;
    matched=true;++affected;
    for(int y=0;y<item.mask.height;++y)for(int x=0;x<item.mask.width;++x){
     const size_t i=size_t(y)*item.mask.width+x;
     if(!item.mask.pixels[i])continue;
     const double dx=x+.5-cx,dy=y+.5-cy;
     if(std::abs(dx*c+dy*s)>halfW||std::abs(-dx*s+dy*c)>halfH){item.mask.pixels[i]=0;++removed;}
    }
    item.components=componentsOf(item.mask,item.name);
    item.source+="+saved rough area (same image)";
   }
   if(matched)r.appliedGuides.push_back(guide);
  }
  if(!r.appliedGuides.empty()){
   r.imageReport+="\nTrainingGuides: loaded "+std::to_string(r.appliedGuides.size())+
     " matching image guide(s); constrained "+std::to_string(affected)+" mask(s), removed "+
     std::to_string(removed)+" predicted pixels outside saved areas. No model weights changed.";
   CrashLog::write("Applied "+std::to_string(r.appliedGuides.size())+" same-image TrainingGuides");
  }
 }catch(const std::exception& e){CrashLog::write(std::string("TrainingGuides skipped: ")+e.what());}
 try{
  r.localLearningApplied=applyLearnedModels(scene_.parent_path().parent_path(),im,r);
  if(r.localLearningApplied){
   for(auto& mask:r.masks)if(mask.source.find("approved pixel mask replay")!=std::string::npos||
      mask.source.find("+user-approved mask")!=std::string::npos||
      mask.source.find("validated local pixel learner")!=std::string::npos||
      mask.source.find("approved-mask pixel learner")!=std::string::npos)
       mask.components.clear(); // Avoid allocating a full-image mask per tiny learned fragment.
   r.imageReport+="\nLocal approved-mask learning: applied "+std::to_string(r.localLearningApplied)+
      " approved or validated learned mask(s). ONNX model weights unchanged.";
  }
 }catch(const std::exception& e){CrashLog::write(std::string("Local learning skipped: ")+e.what());}
 const size_t skinSurfacePriors=addSkinSurfaceAtlas(r,im,skin);
 if(skinSurfacePriors)r.imageReport+="\nSkin surface atlas: added "+std::to_string(skinSurfacePriors)+" pixel-evidence cue mask(s); appearance cues are not medical diagnoses.";
 const size_t atlasPriors=addAtlasPriors(r,person,animal,veh,trees);
 if(atlasPriors)r.imageReport+="\nReference atlas: added "+std::to_string(atlasPriors)+" low-confidence guide region(s) constrained to detected subjects; these are priors, not observations.";
 const size_t hiddenProjections=addOccludedSurfaceProjections(r,im,faceView);
 if(hiddenProjections)r.imageReport+="\nHidden-surface projections: "+std::to_string(hiddenProjections)+
  " low-confidence 2D footprints within detected subjects. No unseen pixels, depth, surface detail or geometry were observed; projections are excluded from training.";
 addAnatomyCatalog(r);
 measureMasks(r,im.width,im.height);
 std::ostringstream ss;ss<<"Analysed "<<im.width<<"x"<<im.height<<" using "<<threads_<<" CPU threads + "<<(gpuActive_?"DirectML GPU":"CPU inference")<<". Found "<<(r.masks.size()-hiddenProjections)<<" visible masks, "<<hiddenProjections<<" projected hidden surfaces / "<<([&](){size_t n=0;for(const auto& m:r.masks)n+=m.components.size();return n;})()<<" connected components. Refinement "<<(settings_.enabled?"ON":"OFF")<<". Face model parts "<<faceModelParts<<", detailed AI parts "<<detailParts<<", fallback parts "<<fallbackParts<<". TrainingGuides applied "<<r.appliedGuides.size()<<", approved/learned masks "<<r.localLearningApplied<<".";r.summary=ss.str();const auto finish=Clock::now();r.refinementMs=elapsed(masksEnd,finish);r.totalMs=elapsed(begin,finish);return r;}
void SegmentationEngine::exportMaskPng(const std::filesystem::path&p,const Mask&m){saveMaskPngWic(p,m);}void SegmentationEngine::exportContoursJson(const std::filesystem::path&p,const Mask&m,const std::string&n){std::ofstream f(p);f<<"{\n  \"mask\": \""<<n<<"\",\n  \"boundary_pixels\": [";bool first=true;for(int y=0;y<m.height;y++)for(int x=0;x<m.width;x++)if(m.pixels[size_t(y)*m.width+x]){bool edge=x==0||y==0||x+1==m.width||y+1==m.height||!m.pixels[size_t(y)*m.width+x-1]||!m.pixels[size_t(y)*m.width+x+1]||!m.pixels[size_t(y-1)*m.width+x]||!m.pixels[size_t(y+1)*m.width+x];if(edge){if(!first)f<<',';first=false;f<<'['<<x<<','<<y<<']';}}f<<"]\n}\n";}
