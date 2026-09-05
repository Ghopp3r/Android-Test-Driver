# Keep JNI entry points wired to native side.
-keepclasseswithmembernames class * { native <methods>; }
-keep class com.mydriver.test.NativeBridge { *; }
