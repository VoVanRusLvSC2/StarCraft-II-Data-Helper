#pragma once
#include "core/CatalogProtection.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QString>
#include <functional>
#include <algorithm>

namespace sc2dh::galaxy {
struct Token { QString raw, value; qsizetype start=0, length=0; bool literal=false; };
struct Reference { QString catalog, value, reason; qsizetype start=0, length=0; bool rewritable=false; };
struct Scan { QVector<Reference> references; QVector<Token> opaqueStrings; bool complete=true; };
inline QVector<Token> lex(const QString &text,bool *complete) {
 QVector<Token> tokens;
 for(qsizetype i=0;i<text.size();) {
  if(text[i].isSpace()){++i;continue;}
  if(text.mid(i,2)==QStringLiteral("//")){while(i<text.size() && text[i]!=QLatin1Char('\n'))++i;continue;}
  if(text.mid(i,2)==QStringLiteral("/*")){const auto end=text.indexOf(QStringLiteral("*/"),i+2);if(end<0){*complete=false;break;}i=end+2;continue;}
  const qsizetype start=i;
  if(text[i]==QLatin1Char('"')) {
   ++i;bool closed=false;
   while(i<text.size()){if(text[i]==QLatin1Char('\\')){i+=2;continue;}if(text[i++]==QLatin1Char('"')){closed=true;break;}}
   if(!closed){*complete=false;break;}
   const QString raw=text.mid(start,i-start);
   const auto decoded=QJsonDocument::fromJson((QLatin1Char('[')+raw+QLatin1Char(']')).toUtf8()).array();
   if(decoded.size()!=1){*complete=false;break;}
   tokens.append({raw,decoded[0].toString(),start,i-start,true});
  } else {
   if(text[i].isLetterOrNumber() || text[i]==QLatin1Char('_'))while(i<text.size() && (text[i].isLetterOrNumber()||text[i]==QLatin1Char('_')))++i;
   else ++i;
   tokens.append({text.mid(start,i-start),{},start,i-start,false});
  }
 }
 return tokens;
}
inline Scan scan(const QString &text) {
 Scan output;const auto tokens=lex(text,&output.complete);QSet<int> classified;
 QHash<QString,QPair<int,int>> constants;QSet<QString> ambiguous;
 int braces=0;
 for(int i=0;i<tokens.size();++i) {
  if(tokens[i].raw==QStringLiteral("{"))++braces;
  if(tokens[i].raw==QStringLiteral("}"))--braces;
  if(braces!=0 || i+4>=tokens.size() || tokens[i].raw!=QStringLiteral("const") || tokens[i+1].raw!=QStringLiteral("string") || tokens[i+3].raw!=QStringLiteral("="))continue;
  int end=i+4;while(end<tokens.size() && tokens[end].raw!=QStringLiteral(";"))++end;
  const QString name=tokens[i+2].raw;
  if(constants.contains(name))ambiguous.insert(name);else constants[name]={i+4,end};
 }
 // A same-named parameter/local or any assignment defeats global constant proof.
 for(int i=0;i<tokens.size();++i) {
  if(!constants.contains(tokens[i].raw))continue;
  const auto declaration=constants.value(tokens[i].raw);
  if(i==declaration.first-2)continue;
  if((i>0 && tokens[i-1].raw==QStringLiteral("string"))
     || (i+1<tokens.size() && tokens[i+1].raw==QStringLiteral("=")))ambiguous.insert(tokens[i].raw);
 }
 if(braces!=0)output.complete=false;
 int evaluationBudget=4096;
 std::function<bool(int,int,QSet<QString>,QString*,int)> evaluate;
 evaluate=[&](int first,int end,QSet<QString> active,QString *value,int depth) {
  if(--evaluationBudget<0 || depth>32 || active.size()>32 || first>=end)return false;
  if(tokens[first].raw==QStringLiteral("(") && tokens[end-1].raw==QStringLiteral(")")) {
   int parentheses=0;bool wraps=true;for(int i=first;i<end-1;++i){if(tokens[i].raw==QStringLiteral("("))++parentheses;if(tokens[i].raw==QStringLiteral(")"))--parentheses;if(parentheses==0){wraps=false;break;}}
   if(wraps)return evaluate(first+1,end-1,active,value,depth+1);
  }
  QString result;bool expect=true;
  for(int i=first;i<end;++i) {
   if(!expect){if(tokens[i].raw!=QStringLiteral("+"))return false;expect=true;continue;}
   if(tokens[i].literal)result+=tokens[i].value;
   else {const auto name=tokens[i].raw;if(!constants.contains(name)||ambiguous.contains(name)||active.contains(name))return false;
    auto next=active;next.insert(name);QString term;const auto range=constants.value(name);if(!evaluate(range.first,range.second,next,&term,depth+1))return false;result+=term;}
   if(result.size()>4096)return false;
   expect=false;
  }
  if(expect)return false;*value=result;return true;
 };
 for(int i=0;i+1<tokens.size();++i) {
  if(tokens[i+1].raw!=QStringLiteral("("))continue;
  const QString function=tokens[i].raw;
  const bool unit=function==QStringLiteral("UnitCreate");
  const bool catalog=function==QStringLiteral("CatalogFieldValueGet")||function==QStringLiteral("CatalogFieldValueSet")||function==QStringLiteral("CatalogFieldValueGetAsInt");
  const bool display=function==QStringLiteral("StringToText");
  if(!unit && !catalog && !display)continue;
  QVector<QPair<int,int>> arguments;int depth=0,first=i+2,end=first;
  for(;end<tokens.size();++end) {
   const QString raw=tokens[end].raw;
   if(raw==QStringLiteral("(")||raw==QStringLiteral("[")||raw==QStringLiteral("{"))++depth;
   if(raw==QStringLiteral(")") && depth==0){arguments.append({first,end});break;}
   if(raw==QStringLiteral(")")||raw==QStringLiteral("]")||raw==QStringLiteral("}"))--depth;
   if(raw==QStringLiteral(",") && depth==0){arguments.append({first,end});first=end+1;}
  }
  if(end==tokens.size()){output.complete=false;continue;}
  if(end+1<tokens.size() && tokens[end+1].raw==QStringLiteral("{")){output.complete=false;continue;}
  const int expectedArguments=unit?6:display?1:function==QStringLiteral("CatalogFieldValueSet")?5:4;
  if(arguments.size()!=expectedArguments){output.complete=false;continue;}
  const auto classify=[&](QPair<int,int> range){for(int j=range.first;j<range.second;++j)if(tokens[j].literal)classified.insert(j);};
  if(display){if(arguments.size()==1)classify(arguments[0]);continue;}
  QString domain;
  if(unit)domain=QStringLiteral("cunit");
  else if(!arguments.isEmpty() && arguments[0].second==arguments[0].first+1) {
   const QString constant=tokens[arguments[0].first].raw;
   const QString prefix=QStringLiteral("c_gameCatalog");
   if(constant.startsWith(prefix))domain=structuralCatalogIdentityScope(QLatin1Char('C')+constant.mid(prefix.size()));
  }
  if(catalog && arguments.size()>2)classify(arguments[2]); // field path is not an entry ID
  if(arguments.size()<2){output.references.append({domain,{},QStringLiteral("Malformed catalog-facing call"),tokens[i].start,0,false});continue;}
  const auto range=arguments[1];QString value;evaluationBudget=4096;const bool resolved=evaluate(range.first,range.second,{},&value,0);
  classify(range);
  Reference ref;ref.catalog=domain;ref.value=resolved?value:QString();ref.start=tokens[i].start;
  ref.reason=resolved?QStringLiteral("Catalog-facing constant expression"):QStringLiteral("Unknown catalog-facing expression");
  if(domain.isEmpty()){ref.value.clear();ref.reason=QStringLiteral("Unknown catalog argument");}
  if(resolved && !domain.isEmpty() && range.second==range.first+1 && tokens[range.first].literal) {
   const auto &token=tokens[range.first];ref.start=token.start+1;ref.length=token.length-2;
   ref.rewritable=token.raw.mid(1,token.raw.size()-2)==value;
   if(ref.rewritable)ref.reason=QStringLiteral("Typed Galaxy literal span");
  }
  output.references.append(ref);
 }
 for(int i=0;i<tokens.size();++i)if(tokens[i].literal && !classified.contains(i))output.opaqueStrings.append(tokens[i]);
 return output;
}
inline QString rewrite(const QString &text,const std::function<QString(const QString&,const QString&)> &replacement,int *count) {
 auto scanResult=scan(text);QString output=text;*count=0;
 if(!scanResult.complete)return output;
 auto refs=scanResult.references;
 std::sort(refs.begin(),refs.end(),[](const Reference &a,const Reference &b){return a.start>b.start;});
 qsizetype last=text.size();
 for(const auto &ref:refs) {
  if(!ref.rewritable || ref.start+ref.length>last)continue;
  const QString next=replacement(ref.catalog,ref.value);
  if(next.isEmpty() || next==ref.value)continue;
  output.replace(ref.start,ref.length,next);last=ref.start;++*count;
 }
 return output;
}
} // namespace sc2dh::galaxy
