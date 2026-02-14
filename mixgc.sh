cd /jdk8u/jdk8u-analysis/test
/jdk8u/jdk8u-analysis/build/linux-x86_64-normal-server-slowdebug/jdk/bin/javac -XX:+UseG1GC G1VerifyMixGC.java
/jdk8u/jdk8u-analysis/build/linux-x86_64-normal-server-slowdebug/jdk/bin/java -Xms4096m -Xmx4096m -XX:+UseG1GC -XX:G1HeapRegionSize=16m -XX:MetaspaceSize=512m -XX:MaxMetaspaceSize=512m -XX:+TraceReferenceGC G1VerifyMixGC