#include "typesdef.h"
#include "osal/string.h"
#include "csi_core.h"

__bobj uint8_t assert_holdup;

void assert_call_addr(const char *__function, unsigned int __line,void* addr)
{
    os_printf(KERN_ERR"%s failed: function: %s, line %d\tcall addr:%X\r\n", __FUNCTION__,__function, __line,(uint32_t)addr);
}
void assert_internal(const char *__function, unsigned int __line, const char *__assertion)
{
    disable_print(0);
    if (assert_holdup) {
        mcu_watchdog_timeout(0); //disable watchdog
        jtag_map_set(1);
    }

    os_printf(KERN_ERR"assertation \"%s\" failed: function: %s, line %d\r\n", __assertion, __function, __line);
    sys_errlog_flush(0xffffffff, 0, 0);

    while(assert_holdup) {
        os_printf("assertation \"%s\" failed: function: %s, line %d\r\n", __assertion, __function, __line);
        delay_us(1000*1000);
    };

    mcu_reset();
}

