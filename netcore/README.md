# Introduction

Netcore support in mono consists of two parts:
* The runtime compiled in netcore mode
* An implementation of System.Private.CoreLib

# Building

Everything below should be executed with the current dir set to 'netcore'.

For bootstrap, do
	./build.sh

To rebuild the runtime, do
	make runtime

To rebuild System.Private.CoreLib, do
	make bcl

These two targets will copy the results into shared/Microsoft.NETCore.App/<version>.

# Running with netcore

## Running through the 'dotnet' tool.

Run ```dotnet publish -c Release -r osx-x64``` to create a published version of the app.
Copy
```mono/mini/.libs/libmonosgen-2.0.dylib```
into
```bin/netcoreapp3.0/osx-x64/publish/libcoreclr.dylib```
Copy
```netcore/System.Private.CoreLib/bin/x86/System.Private.CoreLib.{dll,pdb}```
to
```bin/netcoreapp3.0/osx-x64/publish```

## Running with the mono runtime executable

DYLD_LIBRARY_PATH=shared/Microsoft.NETCore.App/<dotnet version> MONO_PATH=shared/Microsoft.NETCore.App/<dotnet version> ../mono/mini/mono-sgen --assembly-loader=strict sample/HelloWorld/bin/netcoreapp3.0/HelloWorld.dll
