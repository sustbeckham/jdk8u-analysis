cd /jdk8u/jdk8u-analysis/test
/jdk8u/jdk8u-analysis/build/linux-x86_64-normal-server-slowdebug/jdk/bin/javac G1VerifyMixGC.java
/jdk8u/jdk8u-analysis/build/linux-x86_64-normal-server-slowdebug/jdk/bin/java -Xms1024m -Xmx1024m -XX:G1HeapRegionSize=16m -XX:+UseG1GC G1VerifyMixGC