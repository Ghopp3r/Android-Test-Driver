# MyDriverTest report UI

The APK runs the separately provided `/data/local/tmp/my-driver-test` helper and displays its output. It does not contain a JNI driver client or a kernel module. The release package remains `com.mydriver.test`; the debug package is `com.mydriver.test.reportcheck` so report tests can run alongside the existing app.

Every Run All action starts a fresh helper process. The buttons in individual rows also say Run All because the helper executes a complete suite. Clear cancels the active runner and invalidates late results. A process timeout stops the UI waiting; terminating a root helper behind a particular `su` implementation is best effort.

`SuiteReport` requires a complete summary, matching result counts, the expected process exit status, and every subcheck required by a row. Any failed subcheck makes that row FAIL. A skipped subcheck makes an otherwise successful row SKIP. Missing or duplicate results fail validation. Results never persist between executions.

Use Gradle 8.9, JDK 17 or newer, and Android SDK 35. Configure the SDK through your local environment or untracked `local.properties`. NDK and CMake are not needed for this Kotlin-only APK.

```text
gradle -p apk testDebugUnitTest assembleDebug assembleDebugAndroidTest assembleRelease
```

Outputs are under `apk/app/build/outputs/apk/`. The release APK is `release/app-release.apk`; the debug APK is `debug/app-debug.apk`. No automatic copy to the repository root is performed. The shared signing key is only for this internal test application.

The JVM tests and Android instrumentation tests exercise report parsing, process errors, repeated runs, cancellation, and UI state using synthetic process output. They never call the driver or execute `su`. Select a device with `ANDROID_SERIAL` before running:

```text
gradle -p apk connectedDebugAndroidTest
```

The report checks establish that the UI represents the helper output correctly. They do not establish the correctness of the helper's own assertions or the kernel module. Remaining driver findings are recorded in `docs/review-followup.md`.
