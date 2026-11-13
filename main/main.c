#include "sdkconfig.h"
#include "experiments.h"

void legacy_pool_demo_run(void);

void app_main(void)
{
#ifdef CONFIG_APP_LEGACY_POOL_DEMO
    legacy_pool_demo_run();
#else
    adaptive_experiments_run();
#endif
}
