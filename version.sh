cd /jdk8u/jdk8u-analysis/build/linux-x86_64-normal-server-slowdebug/jdk/bin
./java -Xms4096m -Xmx4096m -XX:G1HeapRegionSize=16m -XX:+UseG1GC -XX:MetaspaceSize=512m -XX:MaxMetaspaceSize=512m -version