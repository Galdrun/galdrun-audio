"""Run the actual patched RNG self-test body with failed/zero/repeated inputs."""
from pathlib import Path
import subprocess
import sys
import tempfile


source = (Path(sys.argv[1]) / 'core/drivers/crypto/caam/caam_rng.c').read_text()
start = source.index('void plat_rng_init(void)')
end = source.index('\n#endif', start)
body = source[start:end]
harness = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define TEE_SUCCESS 0
#define IMSG(...) ((void)0)
static int mode, calls;
static void panic(const char *message) { (void)message; exit(7); }
static int do_rng_read(uint8_t *data, size_t size) {
    if (mode == 3) return 1;
    memset(data, mode == 0 ? 0 : mode == 1 ? 42 : ++calls, size);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='connector3-monitor-test-') as directory:
    root = Path(directory)
    (root/'test.c').write_text(harness + body + '\nint main(int argc, char **argv) { (void)argc; mode=atoi(argv[1]); plat_rng_init(); return 0; }\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(root/'test.c'),'-o',str(root/'test')],check=True)
    for mode in range(4):
        result = subprocess.run([str(root/'test'),str(mode)])
        assert result.returncode == (0 if mode == 2 else 7), mode
print('Monitor RNG self-test: read failures, all-zero and repeated samples rejected; distinct samples accepted')
