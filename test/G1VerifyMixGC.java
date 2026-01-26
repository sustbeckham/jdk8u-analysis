import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

public class G1VerifyMixGC {
    static class OldGenObject {
        Object refToYoung;
    }

    private static final List<OldGenObject> OLD_GEN = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        System.out.println("Starting cross-generation reference test...");

        int cycle = 0;
        while (true) {
            createGarbage();

            if (cycle++ % 5 == 0) {
                createCrossGenerationReferences();
            }

            updateReferences();

            TimeUnit.MILLISECONDS.sleep(50);
        }
    }

    private static void createGarbage() {
        for (int i = 0; i < 20; i++) {
            byte[] temp = new byte[512 * 1024]; // 512KB
        }
    }

    private static void createCrossGenerationReferences() {
        OldGenObject oldObj = new OldGenObject();

        oldObj.refToYoung = new byte[200 * 1024];

        OLD_GEN.add(oldObj);
    }

    private static void updateReferences() {
        if (OLD_GEN.size() > 3) {
            OldGenObject obj = OLD_GEN.get(OLD_GEN.size() - 1);
            obj.refToYoung = new byte[100 * 1024];
        }
    }
}
