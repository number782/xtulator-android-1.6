# XTulator Android 1.6 Build - Knowledge File

## Project Overview
- **Repository**: https://github.com/mikechambers84/xtulator
- **Project**: XTulator - x86 PC emulator (80186) written in C
- **Dependencies**: SDL2, libpcap, pthreads, math library
- **Current Build**: Desktop (Visual Studio, GCC/Clang with SDL2)
- **Target**: Android 1.6 Donut (API Level 4)
- **Build System**: Ant (legacy)
- **Approach**: Custom JNI wrapper (not SDL2 Android port)
- **NDK**: User will provide via Docker (need NDK r10e or older for API 4)

## Current Status
- [x] Repository cloned to `/home/andrew/ProjectB/xtulator`
- [x] Project analyzed - C/SDL2 desktop emulator
- [ ] Android NDK environment verified (handled via Docker)
- [x] Android project structure created with Ant build.xml
- [ ] JNI bindings created (Java wrapper + native C interface)
- [ ] Native code compiled for ARM (API 4)
- [ ] APK built and tested

## Tasks
### Task 1: Create Android Project Structure with Ant
- **Assigned To**: Employee 1
- **Status**: COMPLETED
- **Description**: Create Android project directory structure with build.xml, AndroidManifest.xml, project.properties, and jni/ directory for API 4 target
- **Dependencies**: None
- **Output**: Complete Ant project structure in `/home/andrew/ProjectB/xtulator/android/`

### Task 2: Create JNI Wrapper and Java Main Activity
- **Assigned To**: Employee 2
- **Status**: PENDING
- **Description**: Create Java MainActivity and JNI interface to bridge Android UI with C emulator core
- **Dependencies**: Task 1
- **Output**: Java source files in `android/src/`, JNI header in `android/jni/`

### Task 3: Create Android.mk and Application.mk for Native Build
- **Assigned To**: Employee 3
- **Status**: PENDING
- **Description**: Create NDK build files to compile XTulator C sources for ARM API 4
- **Dependencies**: Task 1
- **Output**: Android.mk, Application.mk in `android/jni/`

### Task 4: Adapt C Source for Android (Remove Desktop Dependencies)
- **Assigned To**: Employee 4
- **Status**: PENDING
- **Description**: Modify C sources to remove SDL2/desktop dependencies, use Android native interfaces instead
- **Dependencies**: Task 3
- **Output**: Modified C sources or adapter layer in `android/jni/`

### Task 5: Build and Test APK
- **Assigned To**: Employee 5
- **Status**: PENDING
- **Description**: Run ant debug build, verify APK creation, test on Android 1.6 emulator
- **Dependencies**: Tasks 2, 3, 4
- **Output**: Signed APK in `android/bin/`

## Completed Tasks
*Completed tasks will be moved here*

### Task 1: Create Android Project Structure with Ant - COMPLETED
- **Created**: `/home/andrew/ProjectB/xtulator/android/` directory structure
- **Files created**:
  - `build.xml` - Ant build script targeting API 4 (Android 1.6)
  - `AndroidManifest.xml` - minSdkVersion=4, targetSdkVersion=4, package=com.xtulator.android
  - `project.properties` - target=android-4
  - `default.properties` - Ant build configuration
  - `local.properties` - SDK/NDK path template (requires NDK r10e or older)
  - `build.properties` - User build configuration template
  - `proguard.cfg` - Basic ProGuard config for API 4
  - `src/com/xtulator/android/XTulatorActivity.java` - Main activity with JNI declarations
  - `res/values/strings.xml` - String resources
  - `res/layout/main.xml` - Basic layout
  - `res/drawable/icon.xml` & `icon.png` - App icon (terminal green-on-black)
  - `jni/` - Directory for native code (empty, for Task 3)
  - `assets/README.txt` - Documentation for BIOS/disk images
- **Notes**: 
  - Java source/target set to 1.5 for API 4 compatibility
  - Build.xml includes native build target calling ndk-build
  - Requires NDK r10e or older for API 4 support
  - Ant build system (deprecated but required for API 4)

## Failed Attempts / Blocked Tasks
*Document failed approaches here so future employees don't repeat them*

- None yet

## Known Issues / Blockers
- Android 1.6 (API 4) is very old - modern NDKs don't support it
- SDL2 support for API 4 may not exist - using custom JNI wrapper instead
- Ant build system is deprecated
- NDK r10e or older required for API 4 (user handles via Docker)
- libpcap may not be available on Android 1.6 - NE2000 emulation may need stubbing
- **Target device: SHARP IS01 with physical keyboard & trackball** - can use native key/trackball events instead of on-screen controls

## Employee Notes
*Employees should read this file before starting work and update on completion*

**Employee 1 (Task 1)**: Android project structure created successfully. All Ant build files, manifest, resources, and Java main activity with JNI declarations are in place. The jni/ directory is ready for Task 3 (Android.mk/Application.mk). The XTulatorActivity.java includes native method declarations for emulator control (init, run, stop, key/mouse events, pause/resume).