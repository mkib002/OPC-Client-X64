## Description

* An object oriented OPC DA Client SDK/ToolKit written in C++, Both X86/X64 supported, implementing the OPC DA
  specification version 2.05A
* New Features (Compare with the original version)
    * Build with x64 project.
    * Build with MFC.
    * Multithread supported.
    * Connect to server with CLSID.(Hasn't tested)

## Notes

Current master branch contains a huge bug fixes commit from [PR#14](https://github.com/edimetia3d/OPC-Client-X64/pull/14), which might introduce some new bugs. If the master branch went
wrong, you may check release 0.4.1 as a backup.

## Basic Guide

* Install `OPC Core Component x64` is **NECESSARY** if you want to build x64 project, I bundled the `3.0.105.1`
  version (seems more popular).
* You can test the Toolkit with MatrikonOPC Simulation Server.
    * You can get it free at [offical site](https://www.matrikonopc.com/products/opc-drivers/opc-simulation-server.aspx)
      after reg.
    * Start OPC Simulation Server, then build project. Run the demo, input `hostname`, then input `server ID`, it should
      work.
    * If you were to commit a issue, you should give a code sample with the Simulation Server. Since this is the only
      env we can reproduce the error.
* I advice you to use `hostname` instead of `IP address` for reasons below.
    * If you want to access OPC by IP, you have to enable RemoteRegistry service in `services.msc`
    * Also , for UAC problem after Vista, your program must run as admin to avoid some issue.
* There is a convenience wrapper class that may help you connect local server easier.
