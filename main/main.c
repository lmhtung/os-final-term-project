#include "sdkconfig.h"
#include "experiments.h"

void legacy_pool_demo_run(void);
void heap_lab_run(void);

void app_main(void)
{
#ifdef CONFIG_APP_LEGACY_POOL_DEMO
    legacy_pool_demo_run();
#elif defined(CONFIG_APP_INTERACTIVE_HEAP_LAB)
    heap_lab_run();
#else
    adaptive_experiments_run();
#endif
}
