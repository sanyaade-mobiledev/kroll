/**
 * Appcelerator Kroll - licensed under the Apache Public License 2
 * see LICENSE in the root folder for details on the license.
 * Copyright (c) 2008 Appcelerator, Inc. All Rights Reserved.
 */
#include "base.h"
#ifdef OS_OSX
#include <Cocoa/Cocoa.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#if defined(OS_WIN32)
# include <windows.h>
#else
# include <dirent.h>
#endif
#include <errno.h>
#include <vector>
#include <string>

#include "kroll.h"

namespace kroll
{
	Host::Host(int _argc, const char *_argv[]) :
		argc(_argc), argv(_argv)
	{
		char *ti_home = getenv("KR_HOME");
		char *ti_runtime = getenv("KR_RUNTIME");

		if (ti_home == NULL)
		{
			std::cerr << "KR_HOME not defined, aborting." << std::endl;
			exit(1);
		}

		if (ti_runtime == NULL)
		{
			std::cerr << "KR_RUNTIME not defined, aborting." << std::endl;
			exit(1);
		}

		this->appDirectory = std::string(ti_home);
		this->runtimeDirectory = std::string(ti_runtime);
		this->global_object = new StaticBoundObject();

		// link the name of our global variable to ourself so we can reference
		// from global scope directly to get it
		const char *name = GLOBAL_NS_VARNAME;
		SharedPtr<BoundObject> b = global_object;
		SharedValue wrapper = Value::NewObject(b);
		this->global_object->Set(name,wrapper);
		//KR_DECREF(wrapper);
	}

	Host::~Host()
	{
		//KR_DECREF(this->global_object);
	}

	void Host::RegisterModule(std::string& path, Module* module)
	{
		ScopedLock lock(&moduleMutex);
		KR_ADDREF(module);
		modules[path] = module;
	}

	void Host::UnregisterModule(Module* module)
	{
		ScopedLock lock(&moduleMutex);
		std::map<std::string, Module*>::iterator iter = this->modules.find(
				module->GetName());
		if (this->modules.end() != iter)
		{
			Module *p = iter->second;
			this->modules.erase(iter);
			KR_DECREF(p);
		}
		this->modules[module->GetName()] = module;
		KR_ADDREF(module);
	}

	Module* Host::GetModule(std::string& name)
	{
		ScopedLock lock(&moduleMutex);
		std::map<std::string, Module*>::iterator iter = this->modules.find(name);
		if (this->modules.end() == iter) {
			return 0;
		}
		Module *module = iter->second;
		KR_ADDREF(module);
		return module;
	}

	bool Host::HasModule(std::string name)
	{
		ScopedLock lock(&moduleMutex);
		std::map<std::string, Module*>::iterator iter = this->modules.find(name);
		return (this->modules.end() != iter);
	}

	void Host::LoadModules(std::vector<std::string>& files)
	{
		ScopedLock lock(&moduleMutex);
		std::cout << "have " << files.size() << " files" << std::endl;

		std::vector<std::string>::iterator iter = files.begin();
		for (; iter != files.end(); iter++) {
			std::string path = (*iter);
			std::cout << "loading next module: " << path << std::endl;

			// get the module factory
			Module* module = module_creators[path]->CreateModule(path);
			if (module==NULL)
			{
				std::cerr << "Couldn't load module: " << path << ", skipping..." << std::endl;
			}
			else
			{
				module->SetProvider(module_creators[path]);

				std::cout << "module loaded " << module->GetName() << " from " << path
						<< std::endl;

				// register our module
				this->RegisterModule(path, module);

				//we can now release our reference since the host has it
				KR_DECREF(module);
			}
		}
	}

	#if defined(OS_WIN32)
	std::string module_suffix = "module.dll";
	#elif defined(OS_OSX)
	std::string module_suffix = "module.dylib";
	#elif defined(OS_LINUX)
	std::string module_suffix = "module.so";
	#endif

	bool Host::IsModule(std::string& filename)
	{
		bool isModule = (filename.length() > module_suffix.length() && filename.substr(
				filename.length() - module_suffix.length()) == module_suffix);

		std::cout << "IsModule? " << filename << " " << (isModule ? "true" : "false") << std::endl;
		return isModule;
	}

	ModuleProvider* Host::FindModuleProvider(std::string& filename)
	{
		ScopedLock lock(&moduleMutex);
		if (IsModule(filename))
		{
			return this;
		}

		std::vector<ModuleProvider*>::iterator iter;
		for (iter = module_providers.begin(); iter != module_providers.end(); iter++) {
			ModuleProvider *provider = (*iter);
			if (provider != NULL && provider->IsModule(filename)) {
				//std::cout << "Found [" << provider->GetDescription()
				//		<< "] provider for module: " << filename << std::endl;
				return provider;
			}
		}
		return NULL;
	}

	int Host::FindModules(std::string &dir, std::vector<std::string> &files)
	{
		ScopedLock lock(&moduleMutex);
	#if defined(OS_WIN32)

		std::string searchdir = (dir);
		searchdir.append("\\*");

		WIN32_FIND_DATA findFileData;

		HANDLE hFind = FindFirstFile(searchdir.c_str(), &findFileData);

		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					std::string filename = findFileData.cFileName;
					ModuleProvider *p = FindModuleProvider(filename);
					std::string fullpath = dir + "\\" + filename;
					if (p != NULL) {
						module_creators[fullpath] = p;
						files.push_back(fullpath);
					} else {
						invalid_module_files.push_back(fullpath);
					}
				}
			} while (FindNextFile(hFind, &findFileData));
			FindClose(hFind);
			return 0;
		} else {
			return (int)GetLastError();
		}

	#else
		DIR *dp;
		struct dirent *dirp;
		if ((dp = opendir(dir.c_str())) == NULL) {
			return errno;
		}

		while ((dirp = readdir(dp)) != NULL) {
			std::string fn = std::string(dirp->d_name);
			std::string fullpath = dir + "/" + fn;
			if (fn.substr(0, 1) == "." || fn.substr(0, 2) == "..")
				continue;
			ModuleProvider *p = FindModuleProvider(fn);
			if (p != NULL) {
				module_creators[fullpath] = p;
				files.push_back(fullpath);
			} else {
				invalid_module_files.push_back(fullpath);
			}
		}
		closedir(dp);
		return 0;
	#endif
	}

	SharedPtr<StaticBoundObject> Host::GetGlobalObject() {
		return this->global_object;
	}

	void Host::ScanInvalidModuleFiles()
	{
		ScopedLock lock(&moduleMutex);
		std::vector<std::string>::iterator iter = invalid_module_files.begin();

		while (iter != invalid_module_files.end()) {
			//printf("Find Module Provider for: %s\n", (*iter).c_str());
			ModuleProvider *provider = FindModuleProvider(*iter);

			if (provider != NULL) {
				//printf("Creating module from external provider: %s\n", (*iter).c_str());

				Module *module = provider->CreateModule(*iter);
				printf("Module->SetProvider\n");
				module->SetProvider(provider);

				//printf("Registering module from external provider: %s\n", (*iter).c_str());
				printf("Register Module\n");
				RegisterModule(*iter, module);

				iter = invalid_module_files.erase(iter);
				KR_DECREF(module);
			}
			else {
				iter++;
			}
		}
	}
}

// this is the platform specific code for main thread processing

#ifdef OS_OSX
@interface KrollMainThreadCaller : NSObject
{
	SharedPtr<kroll::BoundMethod> *method;
	SharedPtr<kroll::Value> *result;
	SharedPtr<kroll::ValueList> *args;
}
- (id)initWithBoundMethod:(SharedPtr<kroll::BoundMethod>)method args:(ValueList*)args;
- (void)call;
- (SharedPtr<kroll::Value>)getResult;
@end

@implementation KrollMainThreadCaller
- (id)initWithBoundMethod:(SharedPtr<kroll::BoundMethod>)m args:(ValueList*)a
{
	self = [super init];
	if (self)
	{
		method = new SharedPtr<kroll::BoundMethod>(m);
		args = new SharedPtr<kroll::ValueList>(a);
		result = new SharedPtr<kroll::Value>();
		//KR_ADDREF(method);
	}
	return self;
}
- (void)dealloc
{
	//KR_DECREF(method);
	//KR_DECREF(result);
	delete method;
	delete result;
	delete args;
	[super dealloc];
}
- (SharedPtr<kroll::Value>)getResult
{
	return *result;
}
- (void)call
{
	kroll::ValueList a;
	if (!args->isNull())
	{
		ValueList::iterator i = (*args)->begin();
		while (i!=(*args)->end())
		{
			a.push_back((*i++));
		}
	}
	result->assign((*method)->Call(a));
	//KR_ADDREF(result);
}
@end
#endif

namespace kroll
{
	SharedValue InvokeMethodOnMainThread(SharedBoundMethod method, ValueList* args)
	{
#ifdef OS_OSX
	    KrollMainThreadCaller *caller = [[KrollMainThreadCaller alloc] initWithBoundMethod:method args:args];
	    [caller performSelectorOnMainThread:@selector(call) withObject:nil waitUntilDone:YES];
		SharedValue result = [caller getResult];
		// make sure to return a new reference because we'll release it
		// when we release the caller
		//if (result) KR_ADDREF(result);
		[caller release];
#else
		//FIXME - implement for Win32 and Linux. Until then...we
		//will just forward on same thread
		std::cerr << "WARNING: Invoking method on non-main Thread!" << std::endl;
		SharedValue result = method->Call(*args);
#endif
		return result;
	}
}
