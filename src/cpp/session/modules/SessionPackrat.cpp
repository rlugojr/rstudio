/*
 * SessionPackrat.cpp
 *
 * Copyright (C) 2009-14 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionPackrat.hpp"

#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/Hash.hpp>
#include <core/system/FileMonitor.hpp>
#include <core/RecursionGuard.hpp>

#include <r/RExec.hpp>
#include <r/RJson.hpp>
#include <r/session/RClientState.hpp>

#include <session/projects/SessionProjects.hpp>
#include <session/SessionAsyncRProcess.hpp>
#include <session/SessionModuleContext.hpp>
#include <session/SessionUserSettings.hpp>

#include "SessionPackages.hpp"
#include "session-config.h"

using namespace core;

#ifdef TRACE_PACKRAT_OUTPUT
#define PACKRAT_TRACE(x) \
   std::cerr << "(packrat) " << x << std::endl;
#else
#define PACKRAT_TRACE(x) 
#endif


namespace session {

namespace {

bool isRequiredPackratInstalled()
{
   return module_context::isPackageVersionInstalled("packrat", "0.2.0.100");
}

} // anonymous namespace

namespace modules { 
namespace packrat {

namespace {

// Library and lockfile hashing and comparison -------------------------------

enum PackratHashType
{
   HASH_TYPE_LOCKFILE = 0,
   HASH_TYPE_LIBRARY = 1
};

enum PendingSnapshotAction
{
   SET_PENDING_SNAPSHOT = 0,
   COMPLETE_SNAPSHOT = 1
};

std::string keyOfHashType(PackratHashType hashType)
{
   return hashType == HASH_TYPE_LOCKFILE ?
      "packratLockfileHash" : 
      "packratLibraryHash";
}

std::string getStoredHash(PackratHashType hashType)
{
   json::Value hash = 
      r::session::clientState().getProjectPersistent("packrat",
                                                     keyOfHashType(hashType));
   if (hash.type() == json::StringType) 
      return hash.get_str();
   else
      return "";
}

void setStoredHash(PackratHashType hashType, const std::string& hash)
{
   PACKRAT_TRACE("updating " << keyOfHashType(hashType) << " -> " <<  hash);
   r::session::clientState().putProjectPersistent(
         "packrat", 
         keyOfHashType(hashType), 
         hash);
}

// adds content from the given file to the given file if it's a 
// DESCRIPTION file (used to summarize library content for hashing)
bool addDescContent(int level, const FilePath& path, std::string* pDescContent)
{
   std::string newDescContent;
   if (path.filename() == "DESCRIPTION") 
   {
      Error error = readStringFromFile(path, &newDescContent);
      pDescContent->append(newDescContent);
   }
   return true;
}

// computes a hash of the content of all DESCRIPTION files in the Packrat
// private library
std::string computeLibraryHash()
{
   FilePath libraryPath = 
      projects::projectContext().directory().complete("packrat/lib");

   // find all DESCRIPTION files in the library and concatenate them to form
   // a hashable state
   std::string descFileContent;
   libraryPath.childrenRecursive(
         boost::bind(addDescContent, _1, _2, &descFileContent));

   if (descFileContent.empty())
      return "";

   return hash::crc32HexHash(descFileContent);
}

// computes the hash of the current project's lockfile
std::string computeLockfileHash()
{
   FilePath lockFilePath = 
      projects::projectContext().directory().complete("packrat/packrat.lock");

   if (!lockFilePath.exists()) 
      return "";

   std::string lockFileContent;
   Error error = readStringFromFile(lockFilePath, &lockFileContent);
   if (error)
   {
      LOG_ERROR(error);
      return "";
   }
   
   return hash::crc32HexHash(lockFileContent);
}

std::string getComputedHash(PackratHashType hashType)
{
   if (hashType == HASH_TYPE_LOCKFILE)
      return computeLockfileHash();
   else
      return computeLibraryHash();
}

void checkHashes(
      PackratHashType primary, 
      PackratHashType secondary, 
      boost::function<void(const std::string&, const std::string&)> onPrimaryMismatch)
{
   // if a request to check hashes comes in while we're already checking hashes,
   // drop it: it's very likely that the file monitor has discovered a change
   // to a file we've already hashed.
   DROP_RECURSIVE_CALLS;

   std::string oldHash = getStoredHash(primary);
   std::string newHash = getComputedHash(primary);

   // hashes match, no work needed
   if (oldHash == newHash)
      return;

   // primary hashes mismatch, secondary hashes match
   else if (getStoredHash(secondary) == getComputedHash(secondary)) 
   {
      onPrimaryMismatch(oldHash, newHash);
   }

   // primary and secondary hashes mismatch
   else 
   {
      // TODO: don't do this until the user has resolved any conflicts that
      // may exist, and packrat::status() is clean
      setStoredHash(primary, newHash);
      setStoredHash(secondary, getComputedHash(secondary));
   }
}

// Auto-snapshot -------------------------------------------------------------

// forward declarations
void performAutoSnapshot(const std::string& targetHash);
void pendingSnapshot(PendingSnapshotAction action);

class AutoSnapshot: public async_r::AsyncRProcess
{
public:
   static boost::shared_ptr<AutoSnapshot> create(
         const FilePath& projectDir, 
         const std::string& targetHash)
   {
      boost::shared_ptr<AutoSnapshot> pSnapshot(new AutoSnapshot());
      std::string snapshotCmd;
      Error error = r::exec::RFunction(
            ".rs.getAutoSnapshotCmd",
            projectDir.absolutePath()).call(&snapshotCmd);
      if (error)
         LOG_ERROR(error); // will also be reported in the console

      PACKRAT_TRACE("starting auto snapshot, R command: " << snapshotCmd);
      pSnapshot->setTargetHash(targetHash);
      pSnapshot->start(snapshotCmd.c_str(), projectDir);
      return pSnapshot;
   }

   std::string getTargetHash()
   {
      return targetHash_;
   }
  
private:
   void setTargetHash(const std::string& targetHash)
   {
      targetHash_ = targetHash;
   }

   void onStderr(const std::string& output)
   {
      PACKRAT_TRACE("(auto snapshot) " << output);
   }

   void onStdout(const std::string& output)
   {
      PACKRAT_TRACE("(auto snapshot) " << output);
   }
   
   void onCompleted(int exitStatus)
   {
      PACKRAT_TRACE("finished auto snapshot, exit status = " << exitStatus);
      if (exitStatus != 0)
         return;
      pendingSnapshot(COMPLETE_SNAPSHOT);
   }

   std::string targetHash_;
};

void pendingSnapshot(PendingSnapshotAction action)
{
   static int pendingSnapshots = 0;
   if (action == SET_PENDING_SNAPSHOT)
   {
      pendingSnapshots++;
      PACKRAT_TRACE("snapshot requested while running, queueing ("
                    << pendingSnapshots << ")");
      return;
   }
   else if (action == COMPLETE_SNAPSHOT)
   {
      if (pendingSnapshots > 0)
      {
         PACKRAT_TRACE("executing pending snapshot");
         pendingSnapshots = 0;
         performAutoSnapshot(computeLibraryHash());
      }
      else
      {
         // library and lockfile are now in sync
         setStoredHash(HASH_TYPE_LOCKFILE, computeLockfileHash());
         setStoredHash(HASH_TYPE_LIBRARY, computeLibraryHash());

         // let the client know that it needs to refresh the list of packages
         // (this will also fetch the newly snapshotted status from packrat)
         ClientEvent event(client_events::kInstalledPackagesChanged);
         module_context::enqueClientEvent(event);
      }
   }
}

void performAutoSnapshot(const std::string& newHash)
{
   static boost::shared_ptr<AutoSnapshot> pAutoSnapshot;
   if (pAutoSnapshot && 
       pAutoSnapshot->isRunning())
   {
      // is the requested snapshot for the same state we're already 
      // snapshotting? if it is, ignore the request
      if (pAutoSnapshot->getTargetHash() == newHash)
      {
         PACKRAT_TRACE("snapshot already running (" << newHash << ")");
         return;
      }
      else
      {
         pendingSnapshot(SET_PENDING_SNAPSHOT);
         return;
      }
   }

   // start a new auto-snapshot
   pAutoSnapshot = AutoSnapshot::create(
         projects::projectContext().directory(),
         newHash);
}

// Library and lockfile monitoring -------------------------------------------

void onLockfileUpdate(const std::string& oldHash, const std::string& newHash)
{
   // check to see if there are any restore actions pending 
   SEXP actions;
   r::sexp::Protect protect;
   Error error = r::exec::RFunction(".rs.pendingRestoreActions", 
         projects::projectContext().directory().absolutePath())
         .call(&actions, &protect);

   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   if (r::sexp::length(actions) == 0)
   {
      PACKRAT_TRACE("no pending restore actions found, updating hash");
      setStoredHash(HASH_TYPE_LOCKFILE, newHash);
   }
   else
   {
      PACKRAT_TRACE("found pending restore actions, alerting client");
      json::Value restoreActions;
      r::json::jsonValueFromObject(actions, &restoreActions);
      ClientEvent event(client_events::kPackratRestoreNeeded, restoreActions);
      module_context::enqueClientEvent(event);
   }
}

void onLibraryUpdate(const std::string& oldHash, const std::string& newHash)
{
   performAutoSnapshot(newHash);
}

void onFileChanged(FilePath sourceFilePath)
{
   // we only care about mutations to files in the Packrat library directory
   // (and packrat.lock)
   FilePath libraryPath = 
      projects::projectContext().directory().complete("packrat/lib");

   if (sourceFilePath.filename() == "packrat.lock")
   {
      PACKRAT_TRACE("detected change to lockfile " << sourceFilePath);
      checkHashes(HASH_TYPE_LOCKFILE, HASH_TYPE_LIBRARY, onLockfileUpdate);
   }
   else if (sourceFilePath.isWithin(libraryPath) && 
            (sourceFilePath.isDirectory() || 
             sourceFilePath.filename() == "DESCRIPTION"))
   {
      // ignore changes in the RStudio-managed manipulate and rstudio 
      // directories and the files within them
      if (sourceFilePath.filename() == "manipulate" ||
          sourceFilePath.filename() == "rstudio" ||
          sourceFilePath.parent().filename() == "manipulate" || 
          sourceFilePath.parent().filename() == "rstudio")
      {
         return;
      }
      PACKRAT_TRACE("detected change to library file " << sourceFilePath);
      checkHashes(HASH_TYPE_LIBRARY, HASH_TYPE_LOCKFILE, onLibraryUpdate);
   }
}

void onPackageLibraryMutated()
{
   // make sure a Packrat library exists (we don't care about monitoring 
   // mutations to other libraries)
   FilePath libraryPath = 
      projects::projectContext().directory().complete("packrat/lib");
   if (libraryPath.exists())
   {
      PACKRAT_TRACE("detected user modification to library");
      checkHashes(HASH_TYPE_LIBRARY, HASH_TYPE_LOCKFILE, onLibraryUpdate);
   }
}

void onFilesChanged(const std::vector<core::system::FileChangeEvent>& changes)
{
   BOOST_FOREACH(const core::system::FileChangeEvent& fileChange, changes)
   {
      FilePath changedFilePath(fileChange.fileInfo().absolutePath());
      onFileChanged(changedFilePath);
   }
}

// RPC -----------------------------------------------------------------------

Error installPackrat(const json::JsonRpcRequest& request,
                    json::JsonRpcResponse* pResponse)
{
   Error error = module_context::installEmbeddedPackage("packrat");
   if (error)
   {
      std::string desc = error.getProperty("description");
      if (desc.empty())
         desc = error.summary();

      module_context::consoleWriteError(desc + "\n");

      LOG_ERROR(error);
   }

   pResponse->setResult(!error);

   return Success();
}

Error getPackratPrerequisites(const json::JsonRpcRequest& request,
                              json::JsonRpcResponse* pResponse)
{
   json::Object prereqJson;
   prereqJson["build_tools_available"] = module_context::canBuildCpp();
   prereqJson["package_available"] = isRequiredPackratInstalled();
   pResponse->setResult(prereqJson);
   return Success();
}


Error getPackratContext(const json::JsonRpcRequest& request,
                        json::JsonRpcResponse* pResponse)
{
   pResponse->setResult(module_context::packratContextAsJson());
   return Success();
}

Error packratBootstrap(const json::JsonRpcRequest& request,
                       json::JsonRpcResponse* pResponse)
{
   // read params
   std::string dir;
   Error error = json::readParams(request.params, &dir);
   if (error)
      return error;

   // convert to file path then to system encoding
   FilePath dirPath = module_context::resolveAliasedPath(dir);
   dir = string_utils::utf8ToSystem(dirPath.absolutePath());

   // bootstrap
   error = r::exec::RFunction("packrat:::bootstrap", dir).call();
   if (error)
      LOG_ERROR(error); // will also be reported in the console

   // fire installed packages changed
   ClientEvent event(client_events::kInstalledPackagesChanged);
   module_context::enqueClientEvent(event);

   // return status
   return Success();
}

} // anonymous namespace

json::Object contextAsJson(const module_context::PackratContext& context)
{
   json::Object contextJson;
   contextJson["available"] = context.available;
   contextJson["applicable"] = context.applicable;
   contextJson["packified"] = context.packified;
   contextJson["mode_on"] = context.modeOn;
   return contextJson;
}

json::Object contextAsJson()
{
   module_context::PackratContext context = module_context::packratContext();
   return contextAsJson(context);
}

Error initialize()
{
   using boost::bind;
   using namespace module_context;

   // listen for changes to the project files 
   session::projects::FileMonitorCallbacks cb;
   cb.onFilesChanged = onFilesChanged;
   projects::projectContext().subscribeToFileMonitor("Packrat", cb);
   module_context::events().onSourceEditorFileSaved.connect(onFileChanged);
   module_context::events().onPackageLibraryMutated.connect(
         onPackageLibraryMutated);

   ExecBlock initBlock;

   initBlock.addFunctions()
      (bind(registerRpcMethod, "install_packrat", installPackrat))
      (bind(registerRpcMethod, "get_packrat_prerequisites", getPackratPrerequisites))
      (bind(registerRpcMethod, "get_packrat_context", getPackratContext))
      (bind(registerRpcMethod, "packrat_bootstrap", packratBootstrap))
      (bind(sourceModuleRFile, "SessionPackrat.R"));

   return initBlock.execute();
}

} // namespace packrat
} // namespace modules

namespace module_context {

PackratContext packratContext()
{
   PackratContext context;

   // NOTE: when we switch to auto-installing packrat we need to update
   // this check to look for R >= whatever packrat requires (we don't
   // need to look for R >= 3.0 as we do for rmarkdown/shiny because
   // build tools will be installed prior to attempting to auto-install
   // the embedded version of packrat

   context.available = isRequiredPackratInstalled();

   context.applicable = context.available &&
                        projects::projectContext().hasProject();

   if (context.applicable)
   {
      FilePath projectDir = projects::projectContext().directory();
      Error error = r::exec::RFunction(
                           "packrat:::checkPackified",
                           /* project = */ projectDir.absolutePath(),
                           /* silent = */ true).call(&context.packified);
      if (error)
         LOG_ERROR(error);

      if (context.packified)
      {
         error = r::exec::RFunction(
                            "packrat:::isPackratModeOn",
                            projectDir.absolutePath()).call(&context.modeOn);
         if (error)
            LOG_ERROR(error);
      }
   }

   return context;
}


json::Object packratContextAsJson()
{
   return modules::packrat::contextAsJson();
}

namespace {

void copyOption(SEXP optionsSEXP, const std::string& listName,
                json::Object* pOptionsJson, const std::string& jsonName,
                bool defaultValue)
{
   bool value = defaultValue;
   Error error = r::sexp::getNamedListElement(optionsSEXP,
                                              listName,
                                              &value,
                                              defaultValue);
   if (error)
   {
      error.addProperty("option", listName);
      LOG_ERROR(error);
   }

   (*pOptionsJson)[jsonName] = value;
}

json::Object defaultPackratOptions()
{
   json::Object optionsJson;
   optionsJson["mode_on"] = false;
   optionsJson["auto_snapshot"] = true;
   optionsJson["vcs_ignore_lib"] = true;
   optionsJson["vcs_ignore_src"] = false;
   return optionsJson;
}

} // anonymous namespace

json::Object packratOptionsAsJson()
{
   PackratContext context = packratContext();
   if (context.packified)
   {
      // create options to return and record mode
      json::Object optionsJson;
      optionsJson["mode_on"] = context.modeOn;

      // get the options from packrat
      FilePath projectDir = projects::projectContext().directory();
      r::exec::RFunction getOpts("packrat:::get_opts");
      getOpts.addParam("simplify", false);
      getOpts.addParam("project", module_context::createAliasedPath(
                                                            projectDir));
      r::sexp::Protect rProtect;
      SEXP optionsSEXP;
      Error error = getOpts.call(&optionsSEXP, &rProtect);
      if (error)
      {
         LOG_ERROR(error);
         return defaultPackratOptions();
      }

      // copy the options into json
      copyOption(optionsSEXP, "auto.snapshot",
                 &optionsJson, "auto_snapshot", true);

      copyOption(optionsSEXP, "vcs.ignore.lib",
                 &optionsJson, "vcs_ignore_lib", true);

      copyOption(optionsSEXP, "vcs.ignore.src",
                 &optionsJson, "vcs_ignore_src", false);

      return optionsJson;
   }
   else
   {
      return defaultPackratOptions();
   }
}



} // namespace module_context
} // namespace session

