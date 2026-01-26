import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

public class G1VerifyMixGC {
    // 老年代对象持有新生代对象的引用
    static class OldGenObject {
        Object refToYoung;  // 指向新生代对象的引用
    }

    // 老年代对象集合
    private static final List<OldGenObject> OLD_GEN = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        System.out.println("Starting cross-generation reference test...");

        int cycle = 0;
        while (true) {
            // 1. 创建临时对象（垃圾）
            createGarbage();

            // 2. 定期创建老年代对象并引用新生代对象
            if (cycle++ % 5 == 0) {  // 每2.5秒执行一次
                createCrossGenerationReferences();
            }

            // 3. 更新部分引用（模拟引用变化）
            updateReferences();

            TimeUnit.MILLISECONDS.sleep(50);
        }
    }

    private static void createGarbage() {
        // 创建短期垃圾对象（约10MB）
        for (int i = 0; i < 20; i++) {
            byte[] temp = new byte[512 * 1024]; // 512KB
        }
    }

    private static void createCrossGenerationReferences() {
        // 创建老年代对象（会存活足够长时间进入老年代）
        OldGenObject oldObj = new OldGenObject();

        // 创建被引用的新生代对象（约200KB）
        oldObj.refToYoung = new byte[200 * 1024];

        // 添加到老年代集合
        OLD_GEN.add(oldObj);
    }

    private static void updateReferences() {
        // 每10个周期更新一次引用
        if (OLD_GEN.size() > 3) {
            // 随机选择一个老年代对象更新其引用
            OldGenObject obj = OLD_GEN.get(OLD_GEN.size() - 1);
            obj.refToYoung = new byte[100 * 1024]; // 新创建100KB对象
        }
    }
}
