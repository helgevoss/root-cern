// @(#)root/utils/src:$Id$
// Author: Philippe Canal November 2011 ; originated from Zdenek Culik   16/04/2010 and Velislava Spasova.

/*************************************************************************
 * Copyright (C) 1995-2011, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/rootcint.            *
 *************************************************************************/

#include "Scanner.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallSet.h"
#include "clang/Sema/Sema.h"
#include "clang/Frontend/CompilerInstance.h"

#include "cling/Interpreter/Interpreter.h"

#include "TClassEdit.h"

#include <iostream>
#include <sstream> // class ostringstream

#include "SelectionRules.h"

//#define DEBUG

#define SHOW_WARNINGS
// #define SHOW_TEMPLATE_INFO

// #define COMPLETE_TEMPLATES
// #define CHECK_TYPES

#define FILTER_WARNINGS
#define DIRECT_OUTPUT

// SHOW_WARNINGS - enable warnings
// SHOW_TEMPLATE_INFO - enable informations about encoutered tempaltes

// COMPLETE_TEMPLATES - process templates, not only specializations (instantiations)

// FILTER_WARNINGS -- do not repeat same type of warning
// DIRECT_OUTPUT -- output to std err with gcc compatible filename an line number

// #define SELECTION_DEBUG



namespace {
   
   class RPredicateIsSameNamespace
   {
   private:
      clang::NamespaceDecl *fTarget;
   public:
      RPredicateIsSameNamespace(clang::NamespaceDecl *target) : fTarget(target) {}
      
      bool operator()(const RScanner::AnnotatedNamespaceDecl& element)
      {
         return (fTarget == element);
      }
   };   
   
}
   
using namespace ROOT;
using namespace clang;

extern cling::Interpreter *gInterp;

const char* RScanner::fgClangDeclKey = "ClangDecl"; // property key used for connection with Clang objects
const char* RScanner::fgClangFuncKey = "ClangFunc"; // property key for demangled names

int RScanner::fgAnonymousClassCounter = 0;
int RScanner::fgBadClassCounter = 0;
int RScanner::fgAnonymousEnumCounter  = 0;

std::map <clang::Decl*, std::string> RScanner::fgAnonymousClassMap;
std::map <clang::Decl*, std::string> RScanner::fgAnonymousEnumMap;

//______________________________________________________________________________
RScanner::RScanner (const SelectionRules &rules, const cling::Interpreter &interpret, const ROOT::TMetaUtils::TNormalizedCtxt &normCtxt, unsigned int verbose /* = 0 */) : 
  fVerboseLevel(verbose),
  fSourceManager(0),
  fInterpreter(interpret),
  fRecordDeclCallback(0),
  fNormCtxt(normCtxt),
  fSelectionRules(rules)  
{
   // Regular constructor setting up the scanner to search for entities
   // matching the 'rules'.

   for (int i = 0; i <= fgDeclLast; i ++)
      fDeclTable [i] = false;
   
   for (int i = 0; i <= fgTypeLast; i ++)
      fTypeTable [i] = false;
   
   fLastDecl = 0;
}

//______________________________________________________________________________
RScanner::~RScanner ()
{
}

//______________________________________________________________________________
inline void* ToDeclProp(clang::Decl* item)
{
   /* conversion and type check used by AddProperty */
   return item;
}

//______________________________________________________________________________
inline size_t APIntToSize(const llvm::APInt& num)
{
   return *num.getRawData();
}

//______________________________________________________________________________
inline long APIntToLong(const llvm::APInt& num)
{
   return *num.getRawData();
}

//______________________________________________________________________________
inline std::string APIntToStr(const llvm::APInt& num)
{
   return num.toString(10, true);
}

//______________________________________________________________________________
inline std::string IntToStr(int num)
{
   std::string txt = "";
   txt += num;
   return txt;
}

//______________________________________________________________________________
inline std::string IntToStd(int num)
{
   std::ostringstream stream;
   stream << num;
   return stream.str();
}

//______________________________________________________________________________
inline std::string Message(const std::string &msg, const std::string &location)
{
   std::string loc = location;
   
#ifdef DIRECT_OUTPUT
   int n = loc.length ();
   while (n > 0 && loc [n] != ':')
      n--;
   if (n > 0)
      loc = loc.substr (0, n) + ":";
#endif
   
   if (loc == "")
      return msg;
   else
      return loc + " " + msg;
}

//______________________________________________________________________________
void RScanner::ShowInfo(const std::string &msg, const std::string &location) const
{
   const std::string message = Message(msg, location);
#ifdef DIRECT_OUTPUT
   std::cout << message << std::endl;
#else
   fReporter->Info("RScanner:ShowInfo", "CLR %s", message.Data());
#endif
}

//______________________________________________________________________________
void RScanner::ShowWarning(const std::string &msg, const std::string &location) const
{
#ifdef SHOW_WARNINGS
   const std::string message = Message(msg, location);
#ifdef DIRECT_OUTPUT
   std::cout << message << std::endl;
#else
   fReporter->Warning("RScanner:ShowWarning", "CLR %s", message.Data());
#endif
#endif
}

//______________________________________________________________________________
void RScanner::ShowError(const std::string &msg, const std::string &location) const
{
   const std::string message = Message(msg, location);
#ifdef DIRECT_OUTPUT
   std::cout << message << std::endl;
#else
   fReporter->Error("RScanner:ShowError", "CLR %s", message.Data());
#endif
}

//______________________________________________________________________________
void RScanner::ShowTemplateInfo(const std::string &msg, const std::string &location) const
{
#ifdef SHOW_TEMPLATE_INFO
   std::string loc = location;
   if (loc == "")
      loc = GetLocation (fLastDecl);
   ShowWarning(msg, loc);
#endif
}

//______________________________________________________________________________
std::string RScanner::GetSrcLocation(clang::SourceLocation L) const
{
   std::string location = "";
   llvm::raw_string_ostream stream(location);
   L.print(stream, *fSourceManager);
   return stream.str();
}

//______________________________________________________________________________
std::string RScanner::GetLocation(clang::Decl* D) const
{
   if (D == NULL)
   {
      return "";
   }
   else
   {
      std::string location = "";
      llvm::raw_string_ostream stream(location);
      D->getLocation().print(stream, *fSourceManager);
      return stream.str();
   }
}

//______________________________________________________________________________
std::string RScanner::GetName(clang::Decl* D) const
{
   std::string name = "";
   // std::string kind = D->getDeclKindName();
   
   if (clang::NamedDecl* ND = dyn_cast <clang::NamedDecl> (D)) {
      name = ND->getQualifiedNameAsString();
   }
   
   return name;
}

//______________________________________________________________________________
inline std::string AddSpace(const std::string &txt)
{
   if (txt == "")
      return "";
   else
      return txt + " ";
}

//______________________________________________________________________________
void RScanner::DeclInfo(clang::Decl* D) const
{
   std::string location = GetLocation(D);
   std::string kind = D->getDeclKindName();
   std::string name = GetName(D);
   ShowInfo("Scan: " + kind + " declaration " + name, location);
}

//______________________________________________________________________________
void RScanner::UnknownDecl(clang::Decl* D, const std::string &txt) const
{
   // unknown - this kind of declaration was not known to programmer
   std::string location = GetLocation(D);
   std::string kind = D->getDeclKindName();
   std::string name = GetName(D);
   ShowWarning("Unknown " + AddSpace(txt) + kind + " declaration " + name, location);
}

//______________________________________________________________________________
void RScanner::UnexpectedDecl(clang::Decl* D, const std::string &txt) const
{
   // unexpected - this kind of declaration is unexpected (in concrete place)
   std::string location = GetLocation(D);
   std::string kind = D->getDeclKindName();
   std::string name = GetName(D);
   ShowWarning("Unexpected " + kind + " declaration " + name, location);
}

//______________________________________________________________________________
void RScanner::UnsupportedDecl(clang::Decl* D, const std::string &txt) const
{
   // unsupported - this kind of declaration is probably not used (in current version of C++)
   std::string location = GetLocation(D);
   std::string kind = D->getDeclKindName();
   std::string name = GetName(D);
   ShowWarning("Unsupported " + AddSpace(txt) + kind + " declaration " + name, location);
}

//______________________________________________________________________________
void RScanner::UnimportantDecl(clang::Decl* D, const std::string &txt) const
{
   // unimportant - this kind of declaration is not stored into reflex
}

//______________________________________________________________________________
void RScanner::UnimplementedDecl(clang::Decl* D, const std::string &txt)
{
   // information about item, that should be implemented
   
   clang::Decl::Kind k = D->getKind();
   
   bool show = true;
#ifdef FILTER_WARNINGS
   if (k >= 0 || k <= fgDeclLast) {
      if (fDeclTable [k])
         show = false; // already displayed
      else
         fDeclTable [k] = true;
   }
#endif
   
   if (show)
   {
      std::string location = GetLocation(D);
      std::string kind = D->getDeclKindName();
      std::string name = GetName(D);
      std::string msg = "Unimplemented ";
      if (txt == "") {
         msg +=  "declaration";
      } else {
         msg += txt;
      }
      msg += ": ";
      msg += kind;
      msg += " ";
      msg += name;
      ShowWarning(msg,location);
   }
}

//______________________________________________________________________________
void RScanner::UnknownType(clang::QualType qual_type) const
{
   std::string location = GetLocation(fLastDecl);
   std::string kind = qual_type.getTypePtr()->getTypeClassName();
   ShowWarning("Unknown " + kind + " type " + qual_type.getAsString(), location);
}

//______________________________________________________________________________
void RScanner::UnsupportedType(clang::QualType qual_type) const
{
   std::string location = GetLocation(fLastDecl);
   std::string kind = qual_type.getTypePtr()->getTypeClassName();
   ShowWarning("Unsupported " + kind + " type " + qual_type.getAsString(), location);
}

//______________________________________________________________________________
void RScanner::UnimportantType(clang::QualType qual_type) const
{
   // unimportant - this kind of declaration is not stored into reflex
}

//______________________________________________________________________________
void RScanner::UnimplementedType(clang::QualType qual_type)
{
   clang::Type::TypeClass k = qual_type.getTypePtr()->getTypeClass();
   
   bool show = true;
#ifdef FILTER_WARNINGS
   if (k >= 0 || k <= fgTypeLast) {
      if (fTypeTable [k])
         show = false; // already displayed
      else
         fTypeTable [k] = true;
   }
#endif
   
   if (show)
   {
      std::string location = GetLocation(fLastDecl);
      std::string kind = qual_type.getTypePtr()->getTypeClassName();
      ShowWarning("Unimplemented type: " + kind + " " + qual_type.getAsString(), location);
   }
}

//______________________________________________________________________________
void RScanner::UnimplementedType (const clang::Type* T)
{
   clang::Type::TypeClass k = T->getTypeClass();
   
   bool show = true;
#ifdef FILTER_WARNINGS
   if (k >= 0 || k <= fgTypeLast) {
      if (fTypeTable [k])
         show = false; // already displayed
      else
         fTypeTable [k] = true;
   }
#endif
   
   if (show)
   {
      std::string location = GetLocation(fLastDecl);
      std::string kind = T->getTypeClassName ();
      ShowWarning ("Unimplemented type: " + kind, location);
   }
}

//______________________________________________________________________________
std::string RScanner::GetClassName(clang::RecordDecl* D) const
{
   std::string cls_name = D->getQualifiedNameAsString();
   
   // NO if (cls_name == "")
   // NO if (D->isAnonymousStructOrUnion())
   // NO if (cls_name == "<anonymous>") {
   if (! D->getDeclName ()) {
      if (fgAnonymousClassMap.find (D) != fgAnonymousClassMap.end())
      {
         // already encountered anonymous class
         cls_name = fgAnonymousClassMap [D];
      }
      else
      {
         fgAnonymousClassCounter ++;
         cls_name = "_ANONYMOUS_CLASS_" + IntToStd(fgAnonymousClassCounter) + "_";  // !?
         fgAnonymousClassMap [D] = cls_name;
         // ShowInfo ("anonymous class " + cls_name, GetLocation (D));
      }
   }
   
   return cls_name;
}

//______________________________________________________________________________
std::string RScanner::GetEnumName(clang::EnumDecl* D) const
{
   std::string enum_name = D->getQualifiedNameAsString();
   
   if (! D->getDeclName ()) {
      if (fgAnonymousEnumMap.find (D) != fgAnonymousEnumMap.end())
      {
         // already encountered anonymous enumeration type
         enum_name = fgAnonymousEnumMap [D];
      }
      else
      {
         fgAnonymousEnumCounter ++;
         enum_name = "_ANONYMOUS_ENUM_" + IntToStd(fgAnonymousEnumCounter) + "_";  // !?
         fgAnonymousEnumMap [D] = enum_name;
         // ShowInfo ("anonymous enum " + enum_name, GetLocation (D));
      }
   }
   
   return enum_name;
}

//______________________________________________________________________________
std::string RScanner::ExprToStr(clang::Expr* expr) const
{
   clang::LangOptions lang_opts;
   clang::PrintingPolicy print_opts(lang_opts); // !?
   
   std::string text = "";
   llvm::raw_string_ostream stream(text);
   
   expr->printPretty(stream, NULL, print_opts);
   
   return stream.str();
}

//______________________________________________________________________________
std::string RScanner::ConvTemplateName(clang::TemplateName& N) const
{
   clang::LangOptions lang_opts;
   clang::PrintingPolicy print_opts(lang_opts);  // !?
   
   std::string text = "";
   llvm::raw_string_ostream stream(text);
   
   N.print(stream, print_opts);
   
   return stream.str();
}

#ifdef COMPLETE_TEMPLATES
//______________________________________________________________________________
std::string RScanner::ConvTemplateParameterList(clang::TemplateParameterList* list) const
{
   std::string result = "";
   bool any = false;
   
   for (clang::TemplateParameterList::iterator I = list->begin(), E = list->end(); I != E; ++I) {
      if (any)
         result += ",";
      any = true;
      
      clang::NamedDecl * D = *I;
      
      switch (D->getKind()) {
            
         case clang::Decl::TemplateTemplateParm:
            UnimplementedDecl(dyn_cast <clang::TemplateTemplateParmDecl> (D), "template parameter");
            break;
            
         case clang::Decl::TemplateTypeParm:
         {
            clang::TemplateTypeParmDecl* P = dyn_cast <clang::TemplateTypeParmDecl> (D);
            
            if (P->wasDeclaredWithTypename())
               result += "typename ";
            else
               result += "class ";
            
            if (P->isParameterPack())
               result += "... ";
            
            result += P->getNameAsString();
         }
            break;
            
         case clang::Decl::NonTypeTemplateParm:
         {
            clang::NonTypeTemplateParmDecl* P = dyn_cast <clang::NonTypeTemplateParmDecl> (D);
            result += P->getType().getAsString();
            
            if (clang::IdentifierInfo* N = P->getIdentifier()) {
               result += " ";
               std::string s = N->getName();
               result += s;
            }
            
            if (P->hasDefaultArgument())
               result += " = " + ExprToStr(P->getDefaultArgument());
         }
            break;
            
         default:
            UnknownDecl(*I, "template parameter");
      }
   }
   
   // ShowInfo ("template parameters <" + result + ">");
   
   return "<" + result + ">";
}

//______________________________________________________________________________
std::string RScanner::ConvTemplateParams(clang::TemplateDecl* D)
{
   return ConvTemplateParameterList(D->getTemplateParameters());
}

//______________________________________________________________________________
std::string RScanner::ConvTemplateArguments(const clang::TemplateArgumentList& list)
{
   clang::LangOptions lang_opts;
   clang::PrintingPolicy print_opts(lang_opts);  // !?
   return clang::TemplateSpecializationType::PrintTemplateArgumentList
   (list.data(), list.size(), print_opts);
}
#endif // COMPLETE_TEMPLATES

//______________________________________________________________________________
std::string RScanner::FuncParameters(clang::FunctionDecl* D) const
{
   std::string result = "";
   
   for (clang::FunctionDecl::param_iterator I = D->param_begin(), E = D->param_end(); I != E; ++I) {
      clang::ParmVarDecl* P = *I;
      
      if (result != "")
         result += ";";  // semicolon, not comma, important
      
      std::string type = P->getType().getAsString();
      std::string name = P->getNameAsString();
      
      result += type + " " + name;
      
      // NO if (P->hasDefaultArg ()) // check hasUnparsedDefaultArg () and hasUninstantiatedDefaultArg ()
      if (P->getInit()) {
         std::string init_value = ExprToStr(P->getDefaultArg());
         result += "=" + init_value;
      }
   }
   
   return result;
}

//______________________________________________________________________________
std::string RScanner::FuncParameterList(clang::FunctionDecl* D) const
{
   std::string result = "";
   
   for (clang::FunctionDecl::param_iterator I = D->param_begin(), E = D->param_end(); I != E; ++I) {
      clang::ParmVarDecl* P = *I;
      
      if (result != "")
         result += ",";
      
      std::string type = P->getType().getAsString();
      result += type;
   }
   
   return "(" + result + ")";
}

//______________________________________________________________________________
bool RScanner::VisitNamespaceDecl(clang::NamespaceDecl* N)
{
   // This method visits a namespace node    
   // in case it is implicit we don't create a builder 
   if(N && N->isImplicit()){
      return true;
   }
   
   bool ret = true;

   DumpDecl(N, "");
   
   const ClassSelectionRule *selected = fSelectionRules.IsDeclSelected(N);
   if (selected) {
      
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> true";
#endif
      clang::DeclContext* primary_ctxt = N->getPrimaryContext();
      clang::NamespaceDecl* primary = llvm::dyn_cast<clang::NamespaceDecl>(primary_ctxt);
      
      RPredicateIsSameNamespace pred(primary);
      if ( find_if(fSelectedNamespaces.begin(),fSelectedNamespaces.end(),pred) == fSelectedNamespaces.end() ) {
         // The namespace is not already registered.

         if (fVerboseLevel > 0) {
            std::string qual_name;
            GetDeclQualName(N,qual_name);
            //      std::cout<<"\tSelected namespace -> " << qual_name << " ptr " << (void*)N <<   " decl ctxt " << (void*)N->getPrimaryContext() << " classname " <<primary->getNameAsString() << "\n";
            std::cout<<"\tSelected namespace -> " << qual_name << "\n";
         }
         fSelectedNamespaces.push_back(AnnotatedNamespaceDecl(primary,selected->GetIndex(),selected->RequestOnlyTClass()));
      }
      ret = true;
   }
   else {
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> false";
#endif
   }
   
   // DEBUG if(ret) std::cout<<"\n\tReturning true ...";
   // DEBUG else std::cout<<"\n\tReturning false ...";
   return ret;
}
 
//______________________________________________________________________________
bool RScanner::VisitRecordDecl(clang::RecordDecl* D)
{

   // This method visits a class node

   return TreatRecordDeclOrTypedefNameDecl(D);
   

}

//______________________________________________________________________________
bool RScanner::TreatRecordDeclOrTypedefNameDecl(clang::TypeDecl* typeDecl)
{

   // For every class is created a new class buider irrespectful of weather the
   // class is internal for another class declaration or not.
   // For every class the class builder is put on top of the fClassBuilders
   // stack.
   // RecordDecls and TypedefDecls (or RecordDecls!) are treated.   
   // We follow two different codepaths if the typeDecl is a RecordDecl or
   // a TypedefDecl. If typeDecl is a TypedefDecl, recordDecl becomes the
   // underlying RecordDecl.
   // This is done to leverage the selections rule matching in SelectionRules
   // which works basically with names.
   // At the end of the method, if the typedef name is matched, an AnnotatedRecordDecl
   // with the underlying RecordDecl is fed to the machinery.

   clang::RecordDecl* recordDecl = clang::dyn_cast<clang::RecordDecl>(typeDecl);
   clang::TypedefNameDecl* typedefNameDecl = clang::dyn_cast<clang::TypedefNameDecl>(typeDecl);

   // If typeDecl is not a RecordDecl, try to fetch the RecordDecl behind the TypedefDecl
   if (!recordDecl && typedefNameDecl) {
      recordDecl = ROOT::TMetaUtils::GetUnderlyingRecordDecl(typedefNameDecl->getUnderlyingType());
      }

   // If at this point recordDecl is still NULL, we have a problem
   if (!recordDecl) {
      ROOT::TMetaUtils::Warning("RScanner::TreatRecordDeclOrTypeNameDecl",
       "Could not cast typeDecl either to RecordDecl or could not get RecordDecl underneath typedef.\n");
      return true;
   }

   // At this point, recordDecl must be a RecordDecl pointer.
   
   if (recordDecl && fRecordDeclCallback) {
      // Pass on any declaration.   This is usually used to record dependency.
      // Since rootcint see C++ compliant header files, we can assume that
      // if a forward declaration or declaration has been inserted, the
      // classes for which we are creating a dictionary will be using
      // them either directly or indirectly.   Any false positive can be
      // resolved by removing the spurrious dependency in the (user) header
      // files.
      std::string qual_name;
      GetDeclQualName(recordDecl,qual_name);
      fRecordDeclCallback(qual_name.c_str());
   }

   // in case it is implicit or a forward declaration, we are not interested.
   if(recordDecl && (recordDecl->isImplicit() || !recordDecl->isCompleteDefinition()) ) {
      return true;
   }
   
   // Never select the class templates themselves.
   const clang::CXXRecordDecl *cxxdecl = llvm::dyn_cast<clang::CXXRecordDecl>(recordDecl);
   if (cxxdecl && cxxdecl->getDescribedClassTemplate ()) {
      return true;
   }
   
   DumpDecl(recordDecl, "");
   
   //Test
   const ClassSelectionRule *selectedFromTypedef = typedefNameDecl ? fSelectionRules.IsDeclSelected(typedefNameDecl) : 0;
   const ClassSelectionRule *selectedFromRecDecl = fSelectionRules.IsDeclSelected(recordDecl);
   
   const ClassSelectionRule *selected = typedefNameDecl ? selectedFromTypedef : selectedFromRecDecl;

   if (! selected) return true; // early exit. Nothing more to be done.

   // Selected through typedef but excluded with concrete classname 
   bool excludedFromRecDecl = selectedFromRecDecl && selectedFromRecDecl->GetSelected() == BaseSelectionRule::kNo;

   if (selected->GetSelected() == BaseSelectionRule::kYes && !excludedFromRecDecl) { 
   
      // For the case kNo, we could (but don't) remove the node from the pcm
      // For the case kDontCare, the rule is just a place holder and we are actually trying to exclude some of its children
      // (this is used only in the selection xml case).
      
      // Reject the selection of std::pair on the ground that it is trivial
      // and can easily be recreated from the AST information.
      if (recordDecl && recordDecl->getName() == "pair") {
         const clang::NamespaceDecl *nsDecl = llvm::dyn_cast<clang::NamespaceDecl>(recordDecl->getDeclContext());
         if (!nsDecl){
            ROOT::TMetaUtils::Error("RScanner::TreatRecordDeclOrTypedefNameDecl",
                                    "Cannot convert context of RecordDecl called pair into a namespace.\n");
            return true;
         }
         const clang::NamespaceDecl *nsCanonical = nsDecl->getCanonicalDecl();
         if (nsCanonical && nsCanonical == fInterpreter.getCI()->getSema().getStdNamespace()) {
            if (selected->HasAttributeWithName("file_name") || selected->HasAttributeWithName("file_pattern")) {
               return true;
            }
         }
      }

      // Insert in the selected classes if not already there
      // We need this check since the same class can be selected through its name or typedef         
      if (0 != fselectedRecordDecls.count(recordDecl)){
         return true;
      }
      
      fselectedRecordDecls.insert(recordDecl);

      
      std::string name_value;
      if (selected->GetAttributeValue("name", name_value)) {                  
         ROOT::TMetaUtils::AnnotatedRecordDecl annRecDecl(selected->GetIndex(),
                                                          selected->GetRequestedType(),
                                                          recordDecl,
                                                          name_value.c_str(),
                                                          selected->RequestStreamerInfo(),
                                                          selected->RequestNoStreamer(),
                                                          selected->RequestNoInputOperator(),
                                                          selected->RequestOnlyTClass(),
                                                          selected->RequestedVersionNumber(),
                                                          fInterpreter,
                                                          fNormCtxt);
         fSelectedClasses.push_back(annRecDecl);
      } else {
         ROOT::TMetaUtils::AnnotatedRecordDecl annRecDecl(selected->GetIndex(),
                                                          recordDecl,
                                                          selected->RequestStreamerInfo(),
                                                          selected->RequestNoStreamer(),
                                                          selected->RequestNoInputOperator(),
                                                          selected->RequestOnlyTClass(),
                                                          selected->RequestedVersionNumber(),
                                                          fInterpreter,
                                                          fNormCtxt);
         fSelectedClasses.push_back(annRecDecl);
      }
      
      if (fVerboseLevel > 0) {
         std::string qual_name;
         GetDeclQualName(recordDecl,qual_name);         
         std::string typedef_qual_name;
         std::string typedefMsg;
         if (typedefNameDecl){
            GetDeclQualName(typedefNameDecl,typedef_qual_name);
            typedefMsg = "(through typedef/alias " + typedef_qual_name + ") ";
         }
         
         std::cout <<"Selected class "
         << typedefMsg
         << "-> "
         << qual_name << "\n";
         
      }      
   }

   
   return true;   
}

//______________________________________________________________________________
bool RScanner::VisitTypedefNameDecl(clang::TypedefNameDecl* D)
{
   // Visitor for every TypedefNameDecl, i.e. aliases and typedefs
   // We check three conditions before trying to match the name:
   // 1) If we are using a selection XML
   // 2) If the underlying decl is a RecordDecl
   // 3) If the typedef is eventually contained in the std namespace
   
   
   const clang::DeclContext *ctx = D->getDeclContext();

   bool isInStd=false;
   if (ctx) {
      const clang::NamedDecl *parent = llvm::dyn_cast<clang::NamedDecl> (ctx);
      isInStd = parent && 0 == parent->getQualifiedNameAsString().compare(0,5,"std::");
      }

   if (fSelectionRules.IsSelectionXMLFile() &&
       ROOT::TMetaUtils::GetUnderlyingRecordDecl(D->getUnderlyingType()) &&
       !isInStd){
      TreatRecordDeclOrTypedefNameDecl(D);
   }

    return true;
}

//______________________________________________________________________________
bool RScanner::VisitEnumDecl(clang::EnumDecl* D)
{
   DumpDecl(D, "");
   
   bool ret = true;
   
   if(fSelectionRules.IsDeclSelected(D)) {
      
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> true";
#endif

      clang::DeclContext *ctx = D->getDeclContext();
      
      clang::Decl* parent = dyn_cast<clang::Decl> (ctx);
      if (!parent) {
         ShowWarning("Could not cast parent context to parent Decl","");
         return false;
      }

      std::string qual_name;
      GetDeclQualName(D,qual_name);
      if (fVerboseLevel > 0) std::cout<<"\tSelected enum -> " << qual_name << "\n";

      //if ((fSelectionRules.isSelectionXMLFile() && ctx->isRecord()) || (fSelectionRules.isLinkdefFile() && ctx->isRecord() && fSelectionRules.IsDeclSelected(parent))) {
      if (ctx->isRecord() && fSelectionRules.IsDeclSelected(parent)) {
         //if (ctx->isRecord() && fSelectionRules.IsDeclSelected(parent)){
         
         
         //if (ctx->isRecord()){
         std::string items;
         
         // should we do it that way ?!
         // Here we create a string with all the enum entries in the form of name=value couples
         for (clang::EnumDecl::enumerator_iterator I = D->enumerator_begin(), E = D->enumerator_end(); I != E; ++I) {
            if (items != "")
               items = items + ";";
            items = items + I->getNameAsString();
            items = items + "=" + APIntToStr(I->getInitVal());
            
            //if (I->getInitExpr())
            //items += "=" + APIntToStr(I->getInitVal());
         }
         ret = true;         
      }
      else 
      {
         // ::enum_name, at least according to genreflex output!
         ret = true;
      }
   }
   else {
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> false";
#endif
   }
   
   return ret;
}

//______________________________________________________________________________
bool RScanner::VisitVarDecl(clang::VarDecl* D)
{
   DumpDecl(D, "");
   
   bool ret = true;
   
   if(fSelectionRules.IsDeclSelected(D)){
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> true";
#endif

      if (fVerboseLevel > 0) {
         std::string var_name;      
         var_name = D->getQualifiedNameAsString();
         std::cout<<"\tSelected variable -> " << var_name << "\n";
      }
   }
   else {
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> false";
#endif
   }
   
   return ret;
}

//______________________________________________________________________________
bool RScanner::VisitFieldDecl(clang::FieldDecl* D)
{
   DumpDecl(D, "");
   
   bool ret = true;
   
   if(fSelectionRules.IsDeclSelected(D)){
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> true";
#endif

      // if (fVerboseLevel > 0) {
//      std::string qual_name;
//      GetDeclQualName(D,qual_name);
//      std::cout<<"\tSelected field -> " << qual_name << "\n";
      // }
   }
   else {
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> false";
#endif
   }
   
   return ret;
}

//______________________________________________________________________________
bool RScanner::VisitFunctionDecl(clang::FunctionDecl* D)
{
   DumpDecl(D, "");
   
   bool ret = true;
   
   if(fSelectionRules.IsDeclSelected(D)){
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> true";
#endif
      

      if (fVerboseLevel > 0) {
         // std::string func_name = D->getQualifiedNameAsString() + FuncParameterList(D);
         std::string name;
         clang::DeclContext * ctx = D->getDeclContext();
         clang::Decl* parent = dyn_cast<clang::Decl> (ctx);
         if (!parent) {
            ShowError("in VisitFunctionDecl, could not cast parent context to parent Decl","");
            return false;
         }
         if ((fSelectionRules.IsSelectionXMLFile() && ctx->isRecord()) || (fSelectionRules.IsLinkdefFile() && ctx->isRecord() && fSelectionRules.IsDeclSelected(parent))) {
            //if (ctx->isRecord() && fSelectionRules.IsDeclSelected(parent)){ // Do I need the second part? - Yes - Optimization for Linkdef?
            
            name = D->getNameAsString();
         }
         else{
            name = D->getQualifiedNameAsString();  
         }
         std::cout<<"\tSelected function -> " << name << "\n";
         if (fVerboseLevel > 3) {
            std::string params = FuncParameters(D);
            std::cout<<"\n\tParams are "<<params;
         }
      }
      
   }
   else {
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) std::cout<<"\n\tSelected -> false";
#endif
   }
   
   return ret;
}

//______________________________________________________________________________
bool RScanner::TraverseDeclContextHelper(DeclContext *DC)
{
   bool ret = true;
   
   if (!DC)
      return true;
   
   clang::Decl* D = dyn_cast<clang::Decl>(DC);
   // skip implicit decls
   if (D && D->isImplicit()){
      return true;
   }
   
   for (DeclContext::decl_iterator Child = DC->decls_begin(), ChildEnd = DC->decls_end(); 
        ret && (Child != ChildEnd); ++Child) {      
      ret=TraverseDecl(*Child);
   }
   
   return ret;
   
}

//______________________________________________________________________________
std::string RScanner::GetClassName(clang::DeclContext* DC) const
{
   
   clang::NamedDecl* N=dyn_cast<clang::NamedDecl>(DC);
   std::string ret;
   if(N && (N->getIdentifier()!=NULL))
      ret = N->getNameAsString().c_str();
   
   return ret;
}

//______________________________________________________________________________
void RScanner::DumpDecl(clang::Decl* D, const char* msg) const
{
   if (fVerboseLevel > 3) {
      return;
   }
   std::string name;
   
   if (!D) {
#ifdef SELECTION_DEBUG
      if (fVerboseLevel > 3) printf("\nDEBUG - DECL is NULL: %s", msg);
#endif
      return;
   }
   
   GetDeclName(D, name);
#ifdef SELECTION_DEBUG
   if (fVerboseLevel > 3) std::cout<<"\n\n"<<name<<" -> "<<D->getDeclKindName()<<": "<<msg;
#endif
}

//______________________________________________________________________________
bool RScanner::GetDeclName(clang::Decl* D, std::string& name) const
{
   clang::NamedDecl* N = dyn_cast<clang::NamedDecl> (D);
   
   if (N) {
      name = N->getNameAsString();
      return true;
   }
   else {
      name = "UNNAMED";
      return false;
   }
}

//______________________________________________________________________________
bool RScanner::GetDeclQualName(clang::Decl* D, std::string& qual_name) const
{
   clang::NamedDecl* N = dyn_cast<clang::NamedDecl> (D);
   
   if (N) {
      llvm::raw_string_ostream stream(qual_name);
      N->getNameForDiagnostic(stream,D->getASTContext().getPrintingPolicy(),true); // qual_name = N->getQualifiedNameAsString();
      return true;
   }
   else {
      return false;
   }  
}

//______________________________________________________________________________
bool RScanner::GetFunctionPrototype(clang::Decl* D, std::string& prototype) const {
   if (!D) {
      return false;
   }
   
   clang::FunctionDecl* F = dyn_cast<clang::FunctionDecl> (D);
   
   if (F) {
      
      prototype = "";
      for (clang::FunctionDecl::param_iterator I = F->param_begin(), E = F->param_end(); I != E; ++I) {
         clang::ParmVarDecl* P = *I;
         
         if (prototype != "")
            prototype += ",";
         
         //std::string type = P->getType().getAsString();
         std::string type = P->getType().getAsString();
         if (type.at(type.length()-1) == '*') {
            type.at(type.length()-2) = '*';
            type.erase(type.length()-1);
         }
         prototype += type;
      }
      
      prototype = "(" + prototype + ")";
      return true;
   }
   else {
      ShowWarning("can't convert Decl to FunctionDecl","");
      return false;
   }
}

//______________________________________________________________________________
void RScanner::Scan(const clang::ASTContext &C)
{
   fSourceManager = &C.getSourceManager();
   
//    if (fVerboseLevel >= 3) fSelectionRules.PrintSelectionRules();
   
   if (fVerboseLevel > 0)  {
      if (fSelectionRules.GetHasFileNameRule())
         std::cout<<"File name detected"<<std::endl;
   }

   TraverseDecl(C.getTranslationUnitDecl());

#ifdef SELECTION_DEBUG
   if (!fSelectionRules.AreAllSelectionRulesUsed()) {
      if (fVerboseLevel > 3) std::cout<<"\nDEBUG - unused sel rules"<<std::endl;
   }
#endif
   
   // And finally resort the results according to the rule ordering.
   std::sort(fSelectedClasses.begin(),fSelectedClasses.end());
}


//______________________________________________________________________________
RScanner::DeclCallback RScanner::SetRecordDeclCallback(RScanner::DeclCallback callback)
{
   // Set the callback to the RecordDecl and return the previous one.

   DeclCallback old = fRecordDeclCallback;
   fRecordDeclCallback = callback;
   return old;
}
