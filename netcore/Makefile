ifneq ($(wildcard config.make),)

include config.make

NETCORETESTS_VERSION := $(shell cat ../eng/Versions.props | sed -n 's/.*MicrosoftPrivateCoreFxNETCoreAppVersion>\(.*\)<\/MicrosoftPrivateCoreFxNETCoreAppVersion.*/\1/p' )
NETCOREAPP_VERSION := $(shell cat ../eng/Versions.props | sed -n 's/.*MicrosoftNETCoreAppVersion>\(.*\)<\/MicrosoftNETCoreAppVersion.*/\1/p' )
ROSLYN_VERSION:=  $(shell cat ../eng/Versions.props | sed -n 's/.*MicrosoftNetCompilersVersion>\(.*\)<\/MicrosoftNetCompilersVersion.*/\1/p' )

# Extracted from https://dotnetcli.blob.core.windows.net/dotnet/aspnetcore/Runtime/master/latest.version
ASPNETCOREAPP_VERSION := 3.0.0-preview-18614-0151

# runtime version used by $(DOTNET) - local .net core sdk to bootstrap stuff
# it doesn't match NETCOREAPP_VERSION
BOOTSTRAP_RUNTIME = $(shell ls ../.dotnet/shared/Microsoft.NETCore.App)

ifeq ($(HOST_PLATFORM),win32)
PLATFORM_AOT_SUFFIX := .dll
PLATFORM_AOT_PREFIX :=
NETCORESDK_EXT = zip
UNZIPCMD = python -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1], 'r').extractall()"
XUNIT_FLAGS = -notrait category=nonwindowstests @../../../../CoreFX.issues_windows.rsp
TESTS_PLATFORM = Windows_NT.x64
ON_RUNTIME_EXTRACT = chmod -R 755 {host,shared,./dotnet}
DOTNET := $(shell powershell -ExecutionPolicy Bypass -Command "./init-tools.ps1")/dotnet.exe
DOTNET := "$(subst \,/,$(DOTNET))"
endif

ifeq ($(HOST_PLATFORM),macos)
PLATFORM_AOT_SUFFIX := .dylib
PLATFORM_AOT_PREFIX := lib
NETCORESDK_EXT = tar.gz
UNZIPCMD = tar -xvf
XUNIT_FLAGS = -notrait category=nonosxtests
TESTS_PLATFORM = OSX.x64
DOTNET := $(shell ./init-tools.sh | tail -1)
endif

ifeq ($(HOST_PLATFORM),linux)
PLATFORM_AOT_SUFFIX := .so
PLATFORM_AOT_PREFIX := lib
NETCORESDK_EXT = tar.gz
UNZIPCMD = tar -xvf
XUNIT_FLAGS = -notrait category=nonlinuxtests @../../../../CoreFX.issues_linux.rsp
ifeq ($(COREARCH), arm64)
XUNIT_FLAGS += @../../../../CoreFX.issues_linux_arm64.rsp
endif
TESTS_PLATFORM = Linux.x64
DOTNET := $(shell ./init-tools.sh | tail -1)
endif

NETCORESDK_FILE := dotnet-runtime-$(NETCOREAPP_VERSION)-$(RID).$(NETCORESDK_EXT)
ASPNETCORESDK_FILE := aspnetcore-runtime-$(ASPNETCOREAPP_VERSION)-$(RID).$(NETCORESDK_EXT)
NETCORE_URL := https://dotnetcli.blob.core.windows.net/dotnet/Runtime/$(NETCOREAPP_VERSION)/$(NETCORESDK_FILE)
ASPNETCORE_URL := https://dotnetcli.blob.core.windows.net/dotnet/aspnetcore/Runtime/$(ASPNETCOREAPP_VERSION)/$(ASPNETCORESDK_FILE)
FEED_BASE_URL := https://dotnetfeed.blob.core.windows.net/dotnet-core
TEST_ASSETS_PATH := corefx-tests/$(NETCORETESTS_VERSION)/$(TESTS_PLATFORM)/netcoreapp/corefx-test-assets.xml

# used to calculate exact version number for generating nupkg
BLAME = $(shell git blame ../configure.ac | grep AC_INIT | cut -f1 -d' ')
VERSTUB = .$(shell git rev-list --count $(BLAME)..HEAD)

all: bcl

$(NETCORESDK_FILE):
	curl $(NETCORE_URL) --output $(NETCORESDK_FILE)
	rm -rf shared/Microsoft.NETCore.App
	$(UNZIPCMD) $(NETCORESDK_FILE)
	$(ON_RUNTIME_EXTRACT)

# AspNetCoreApp contains its own .NET Core Runtime but we don't need it so let's remove it 
# and update version in Microsoft.AspNetCore.App.runtimeconfig.json to NETCOREAPP_VERSION
$(ASPNETCORESDK_FILE):
	curl $(ASPNETCORE_URL) --output $(ASPNETCORESDK_FILE)
	$(UNZIPCMD) $(ASPNETCORESDK_FILE)
	sed -e 's/.*version.*/\"version\": \"$(NETCOREAPP_VERSION)\"/' < shared/Microsoft.AspNetCore.App/$(ASPNETCOREAPP_VERSION)/Microsoft.AspNetCore.App.runtimeconfig.json > 2 && mv 2 shared/Microsoft.AspNetCore.App/$(ASPNETCOREAPP_VERSION)/Microsoft.AspNetCore.App.runtimeconfig.json

update-corefx: corefx/.stamp-dl-corefx-$(NETCORETESTS_VERSION)

corefx/.stamp-dl-corefx-$(NETCORETESTS_VERSION): corefx-restore.csproj
	$(DOTNET) build corefx-restore.csproj --runtime $(RID) --packages corefx/packages -p:MicrosoftPrivateCoreFxNETCoreAppVersion=$(NETCORETESTS_VERSION) -p:OutputPath=corefx/restore/
	touch $@

update-roslyn: roslyn/packages/microsoft.net.compilers.toolset/$(ROSLYN_VERSION)/microsoft.net.compilers.toolset.nuspec

roslyn/packages/microsoft.net.compilers.toolset/$(ROSLYN_VERSION)/microsoft.net.compilers.toolset.nuspec:
	$(DOTNET) restore roslyn-restore.csproj -p:RoslynVersion=$(ROSLYN_VERSION) --packages roslyn/packages -p:OutputPath=roslyn/restore/

run-sample: prepare
	$(DOTNET) build sample/HelloWorld
	MONO_ENV_OPTIONS="--debug" COMPlus_DebugWriteToStdErr=1 ./dotnet --fx-version "$(NETCOREAPP_VERSION)" sample/HelloWorld/bin/netcoreapp3.0/HelloWorld.dll

run-sample-coreclr:
	cd sample/HelloWorld && $(DOTNET) run

run-aspnet-sample: prepare
	rm -rf sample/AspNetCore/{bin,obj}
	$(DOTNET) publish sample/AspNetCore -c Debug -r $(RID)
	cp ../mono/mini/.libs/libmonosgen-2.0$(PLATFORM_AOT_SUFFIX) sample/AspNetCore/bin/Debug/netcoreapp3.0/$(RID)/publish/$(PLATFORM_AOT_PREFIX)coreclr$(PLATFORM_AOT_SUFFIX)
	cp System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.dll sample/AspNetCore/bin/Debug/netcoreapp3.0/$(RID)/publish/
	COMPlus_DebugWriteToStdErr=1 ./dotnet --fx-version "$(ASPNETCOREAPP_VERSION)" sample/AspNetCore/bin/Debug/netcoreapp3.0/$(RID)/publish/AspNetCore.dll

# makes $(DOTNET) to use mono runtime (to run real-world apps using '$(DOTNET) run')
patch-local-dotnet: prepare
	cp ../mono/mini/.libs/libmonosgen-2.0$(PLATFORM_AOT_SUFFIX) ../.dotnet/shared/Microsoft.NETCore.App/$(BOOTSTRAP_RUNTIME)/$(PLATFORM_AOT_PREFIX)coreclr$(PLATFORM_AOT_SUFFIX)
	cp System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.dll ../.dotnet/shared/Microsoft.NETCore.App/$(BOOTSTRAP_RUNTIME)
	cp System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.pdb ../.dotnet/shared/Microsoft.NETCore.App/$(BOOTSTRAP_RUNTIME)

# COREHOST_TRACE=1 
SHAREDRUNTIME := shared/Microsoft.NETCore.App/$(NETCOREAPP_VERSION)

System.Private.CoreLib/src/System/Environment.Mono.cs: System.Private.CoreLib/src/System/Environment.Mono.in config.make
	test -n '$(MONO_CORLIB_VERSION)'
	sed -e 's,@''MONO_CORLIB_VERSION@,$(MONO_CORLIB_VERSION),' $< > $@

bcl: update-roslyn System.Private.CoreLib/src/System/Environment.Mono.cs
	$(DOTNET) build $(CORETARGETS) $(CORLIB_BUILD_FLAGS) -p:BuildArch=$(COREARCH) \
	-p:OutputPath=bin/$(COREARCH) \
	-p:RoslynPropsFile="../roslyn/packages/microsoft.net.compilers.toolset/$(ROSLYN_VERSION)/build/Microsoft.Net.Compilers.Toolset.props" \
	System.Private.CoreLib/System.Private.CoreLib.csproj

debug-bcl:
	$(MAKE) bcl CORLIB_BUILD_FLAGS="-c Debug"

runtime:
	$(MAKE) -C ../mono

link-mono: $(SHAREDRUNTIME)/.stamp-link-mono

$(SHAREDRUNTIME)/.stamp-link-mono: ../mono/mini/.libs/libmonosgen-2.0$(PLATFORM_AOT_SUFFIX) System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.dll System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.pdb
	cp ../mono/mini/.libs/libmonosgen-2.0$(PLATFORM_AOT_SUFFIX) $(SHAREDRUNTIME)/$(PLATFORM_AOT_PREFIX)coreclr$(PLATFORM_AOT_SUFFIX)
	cp System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.dll $(SHAREDRUNTIME)
	cp System.Private.CoreLib/bin/$(COREARCH)/System.Private.CoreLib.pdb $(SHAREDRUNTIME)
	touch $@

prepare: $(ASPNETCORESDK_FILE) $(NETCORESDK_FILE) update-corefx update-roslyn link-mono

nupkg:
	$(DOTNET) pack roslyn-restore.csproj -p:IsPackable=true -p:NuspecFile=runtime.nuspec -p:NuspecProperties=\"RID=$(RID)\;VERSION=$(VERSION)$(VERSTUB)\;PLATFORM_AOT_SUFFIX=$(PLATFORM_AOT_SUFFIX)\;COREARCH=$(COREARCH)\;PLATFORM_AOT_PREFIX=$(PLATFORM_AOT_PREFIX)\" --output ../artifacts/ --no-build
	$(DOTNET) pack roslyn-restore.csproj -p:IsPackable=true -p:NuspecFile=metapackage.nuspec -p:NuspecProperties=\"RID=$(RID)\;VERSION=$(VERSION)$(VERSTUB)\;PLATFORM_AOT_SUFFIX=$(PLATFORM_AOT_SUFFIX)\;COREARCH=$(COREARCH)\;PLATFORM_AOT_PREFIX=$(PLATFORM_AOT_PREFIX)\" --output ../artifacts/ --no-build

clean:
	rm -rf .configured ../.dotnet sdk shared host dotnet tests obj corefx roslyn LICENSE.txt ThirdPartyNotices.txt $(NETCORESDK_FILE) $(ASPNETCORESDK_FILE)


update-tests-corefx: corefx/.stamp-dl-corefx-tests-$(NETCORETESTS_VERSION)

corefx/.stamp-dl-corefx-tests-$(NETCORETESTS_VERSION):
	rm -rf corefx/tests
	$(DOTNET) msbuild corefx-tests-restore.proj -m -t:DownloadAllTests -p:TEST_ASSETS_PATH="$(TEST_ASSETS_PATH)" -p:FEED_BASE_URL="$(FEED_BASE_URL)"
	rm -rf corefx/tests/extracted/System.Utf8String.Experimental.Tests
	touch $@

#
# Running CoreFX tests
#
# You can run either individual test assembly or run all CoreFX tests. The tests are automatically downloaded
# from the shared location and built locally.
#
# Running all test: `make run-tests-corefx`
#
# Running individual test: MONO_ENV_OPTIONS="--debug" make run-tests-corefx-System.Collections.Tests
#

run-tests-corefx: update-tests-corefx
	for testdir in corefx/tests/extracted/*; do \
		$(MAKE) run-tests-corefx-$$(basename $${testdir}) || true; \
	done; \
	$(MAKE) xunit-summary

run-tests-corefx-%: prepare update-tests-corefx
	@echo ""
	@echo "***************** $* *********************"
	@cp corefx/restore/corefx-restore.deps.json corefx/tests/extracted/$*/xunit.console.deps.json
	@cp corefx/restore/corefx-restore.deps.json corefx/tests/extracted/$*/Microsoft.DotNet.RemoteExecutorHost.deps.json
	@cp corefx/restore/corefx-restore.runtimeconfig.dev.json corefx/tests/extracted/$*/$*.runtimeconfig.dev.json
	@cp corefx/restore/corefx-restore.dll corefx/tests/extracted/$*/corefx-restore.dll
	@sed -i -e 's/5.0.0\"/$(NETCOREAPP_VERSION)\"/g' corefx/tests/extracted/$*/*.runtimeconfig.json
	cd corefx/tests/extracted/$* && \
	COMPlus_DebugWriteToStdErr=1 $(CURDIR)/./dotnet exec --runtimeconfig $*.runtimeconfig.json --fx-version "$(NETCOREAPP_VERSION)" xunit.console.dll $*.dll \
		-html ../../TestResult-$*.html -xml ../../TestResult-$*-netcore-xunit.xml \
		$(XUNIT_FLAGS) @../../../../CoreFX.issues.rsp \
		$(FIXTURE)

# build a test assembly in COREFX_ROOT (path to a fully built corefx repo) and copy it to corefx/tests/extracted
# e.g. `make build-test-corefx-System.Runtime COREFX_ROOT=/prj/corefx-master`
build-test-corefx-%:
	cd $(COREFX_ROOT)/src/$*/tests && $(DOTNET) msbuild /p:OutputPath=tmp
	cp $(COREFX_ROOT)/src/$*/tests/tmp/$*.Tests.{dll,pdb} $(CURDIR)/corefx/tests/extracted/$*.Tests/
	rm -rf $(COREFX_ROOT)/src/$*/tests/tmp

xunit-summary:
	./xunit-summary.py "corefx/tests"

endif

distdir:
