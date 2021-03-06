//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Vassil Vassilev <vasil.georgiev.vasilev@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/Interpreter/ClangInternalState.h"

#include "clang/AST/ASTContext.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"

#include <cstdio>
#include <string>
#include <time.h>

#ifdef WIN32
#define popen _popen
#define pclose _pclose
#endif

using namespace clang;

namespace cling {

  ClangInternalState::ClangInternalState(ASTContext& AC, Preprocessor& PP,
                                         llvm::Module& M, const std::string& name)
    : m_ASTContext(AC), m_Preprocessor(PP), m_Module(M), 
      m_DiffCommand("diff -u --text "), m_Name(name) {
    store();
  }

  ClangInternalState::~ClangInternalState() {
    // cleanup the temporary files:
    remove(m_LookupTablesFile.c_str());
    remove(m_IncludedFilesFile.c_str());
    remove(m_ASTFile.c_str());
    remove(m_LLVMModuleFile.c_str());
    remove(m_MacrosFile.c_str());
  }

  void ClangInternalState::store() {
    // Cannot use the stack (private copy ctor)
    llvm::OwningPtr<llvm::raw_fd_ostream> m_LookupTablesOS;
    llvm::OwningPtr<llvm::raw_fd_ostream> m_IncludedFilesOS;
    llvm::OwningPtr<llvm::raw_fd_ostream> m_ASTOS;
    llvm::OwningPtr<llvm::raw_fd_ostream> m_LLVMModuleOS;
    llvm::OwningPtr<llvm::raw_fd_ostream> m_MacrosOS;

    m_LookupTablesOS.reset(createOutputFile("lookup",
                                            &m_LookupTablesFile));
    m_IncludedFilesOS.reset(createOutputFile("included",
                                             &m_IncludedFilesFile));
    m_ASTOS.reset(createOutputFile("ast", &m_ASTFile));
    m_LLVMModuleOS.reset(createOutputFile("module", &m_LLVMModuleFile));
    m_MacrosOS.reset(createOutputFile("macros", &m_MacrosFile));

    printLookupTables(*m_LookupTablesOS.get(), m_ASTContext);
    printIncludedFiles(*m_IncludedFilesOS.get(), 
                       m_ASTContext.getSourceManager());
    printAST(*m_ASTOS.get(), m_ASTContext);
    printLLVMModule(*m_LLVMModuleOS.get(), m_Module);
    printMacroDefinitions(*m_MacrosOS.get(), m_Preprocessor);
  }
  namespace {
    std::string getCurrentTimeAsString() {
      time_t rawtime;
      struct tm * timeinfo;
      char buffer [80];

      time (&rawtime);
      timeinfo = localtime (&rawtime);

      strftime (buffer, 80, "%I_%M_%S", timeinfo);
      return buffer;
    }
  }

  // Copied with modifications from CompilerInstance.cpp
  llvm::raw_fd_ostream* 
  ClangInternalState::createOutputFile(llvm::StringRef OutFile,
                                       std::string *TempPathName/*=0*/,
                                       bool RemoveFileOnSignal/*=true*/) {
    llvm::OwningPtr<llvm::raw_fd_ostream> OS;
    std::string OSFile;
    llvm::SmallString<256> OutputPath;
    llvm::sys::path::system_temp_directory(/*erasedOnReboot*/false, OutputPath);

    // Only create the temporary if the parent directory exists (or create
    // missing directories is true) and we can actually write to OutPath,
    // otherwise we want to fail early.
    llvm::SmallString<256> TempPath(OutputPath);
    llvm::sys::fs::make_absolute(TempPath);
    assert(llvm::sys::fs::is_directory(TempPath.str()) && "Must be a folder.");
    // Create a temporary file.
    llvm::sys::path::append(TempPath, OutFile);
    TempPath += "-" + getCurrentTimeAsString();
    TempPath += "-%%%%%%%%";
    int fd;
    if (llvm::sys::fs::createUniqueFile(TempPath.str(), fd, TempPath)
        == llvm::errc::success) {
      OS.reset(new llvm::raw_fd_ostream(fd, /*shouldClose=*/true));
      OSFile = TempPath.str();
    }

    // Make sure the out stream file gets removed if we crash.
    if (RemoveFileOnSignal)
      llvm::sys::RemoveFileOnSignal(OSFile);

    if (TempPathName)
      *TempPathName = OSFile;

    return OS.take();
  }

  void ClangInternalState::compare(ClangInternalState& other) {
    std::string differences = "";
    // Ignore the builtins
    typedef llvm::SmallVector<const char*, 1024> Builtins;
    Builtins builtinNames;
    m_ASTContext.BuiltinInfo.GetBuiltinNames(builtinNames);
    for (Builtins::iterator I = builtinNames.begin(); 
         I != builtinNames.end();) {
      if (llvm::StringRef(*I).startswith("__builtin"))
        I = builtinNames.erase(I);
      else
        ++I;
    }

    builtinNames.push_back(".*__builtin.*");      
    
    if (differentContent(m_LookupTablesFile, other.m_LookupTablesFile, 
                         differences, &builtinNames)) {
      llvm::errs() << "Differences in the lookup tables\n";
      llvm::errs() << differences << "\n";
      differences = "";
    }

    if (differentContent(m_IncludedFilesFile, other.m_IncludedFilesFile, 
                         differences)) {
      llvm::errs() << "Differences in the included files\n";
      llvm::errs() << differences << "\n";
      differences = "";
    }
    if (differentContent(m_ASTFile, other.m_ASTFile, differences)) {
      llvm::errs() << "Differences in the AST \n";
      llvm::errs() << differences << "\n";
      differences = "";
    }

    if (differentContent(m_LLVMModuleFile, other.m_LLVMModuleFile, differences)){
      llvm::errs() << "Differences in the llvm Module \n";
      llvm::errs() << differences << "\n";
      differences = "";
    }
    if (differentContent(m_MacrosFile, other.m_MacrosFile, differences)){
      llvm::errs() << "Differences in the Macro Definitions \n";
      llvm::errs() << differences << "\n";
      differences = "";
    }
  }

  bool ClangInternalState::differentContent(const std::string& file1, 
                                            const std::string& file2, 
                                            std::string& differences,
                const llvm::SmallVectorImpl<const char*>* ignores/*=0*/) const {
    std::string diffCall = m_DiffCommand;
    if (ignores) {
      for (size_t i = 0, e = ignores->size(); i < e; ++i) {
        diffCall += " --ignore-matching-lines=\".*";
        diffCall += (*ignores)[i];
        diffCall += ".*\"";
      }
    }

    FILE* pipe = popen((diffCall + " " + file1 + " " + file2).c_str() , "r");
    assert(pipe && "Error creating the pipe");
    assert(differences.empty() && "Must be empty");

    char buffer[128];
    while(!feof(pipe)) {
      if(fgets(buffer, 128, pipe) != NULL)
        differences += buffer;
    }
    pclose(pipe);

    if (!differences.empty())
      llvm::errs() << diffCall << " " << file1 << " " << file2 << "\n";

    return !differences.empty();
  }

  class DumpLookupTables : public RecursiveASTVisitor<DumpLookupTables> {
  private:
    llvm::raw_ostream& m_OS;
  public:
    DumpLookupTables(llvm::raw_ostream& OS) : m_OS(OS) { }
    bool VisitDecl(Decl* D) {
      if (DeclContext* DC = dyn_cast<DeclContext>(D))
        VisitDeclContext(DC);
      return true;
    }

    bool VisitDeclContext(DeclContext* DC) {
      // If the lookup is pending for building, force its creation.
      if (DC == DC->getPrimaryContext() && !DC->getLookupPtr())
        DC->buildLookup();
      DC->dumpLookups(m_OS);
      return true;
    }
  };

  void ClangInternalState::printLookupTables(llvm::raw_ostream& Out, 
                                             ASTContext& C) {
    DumpLookupTables dumper(Out);
    dumper.TraverseDecl(C.getTranslationUnitDecl());
  }

  void ClangInternalState::printIncludedFiles(llvm::raw_ostream& Out, 
                                              SourceManager& SM) {
    for (clang::SourceManager::fileinfo_iterator I = SM.fileinfo_begin(),
           E = SM.fileinfo_end(); I != E; ++I) {
      const clang::SrcMgr::ContentCache &C = *I->second;
      const clang::FileEntry *FE = C.OrigEntry;
      // Our error recovery purges the cache of the FileEntry, but keeps
      // the FileEntry's pointer so that if it was used by smb (like the
      // SourceManager) it wouldn't be dangling. In that case we shouldn't
      // print the FileName, because semantically it is not there.
      if (!FE->getSize() && !FE->getModificationTime())
        continue;
      std::string fileName(FE->getName());
      if (!(fileName.compare(0, 5, "/usr/") == 0 &&
            fileName.find("/bits/") != std::string::npos)) {
        Out << fileName << '\n';
      }
    }
  }

  void ClangInternalState::printAST(llvm::raw_ostream& Out, ASTContext& C) {
    TranslationUnitDecl* TU = C.getTranslationUnitDecl();
    unsigned Indentation = 0;
    bool PrintInstantiation = false;
    std::string ErrMsg;
    clang::PrintingPolicy policy = C.getPrintingPolicy();
    TU->print(Out, policy, Indentation, PrintInstantiation);
    // TODO: For future when we relpace the bump allocation with slab.
    //
    //Out << "Allocated memory: " << C.getAllocatedMemory();
    //Out << "Side table allocated memory: " << C.getSideTableAllocatedMemory();
    Out.flush();
  }

  void ClangInternalState::printLLVMModule(llvm::raw_ostream& Out, 
                                           llvm::Module& M) {
    M.print(Out, /*AssemblyAnnotationWriter*/ 0);
  }

  void ClangInternalState::printMacroDefinitions(llvm::raw_ostream& Out,
                            clang::Preprocessor& PP) {
    PP.printMacros(Out);
  }
} // end namespace cling
