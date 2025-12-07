/* 我已经将conpact_wifi_board.cc中159行注释了，小智内置开关灯为18号针脚，
   所以可以直接用18号针脚的话，就要将上面的159行的注释恢复；
   如果想用17号针脚，就不要恢复注释了。
 
*/
#include "mcp_server.h"
#include <esp_log.h>
 
#define TAG "LED灯事件"
 
class LED_Lamp  {          // 1. 类名改成 LED_Lamp 
private:
    bool power_ = false;
    gpio_num_t gpio_num_;
 
public: //如果修改类名LED_Lamp，下面一行中的LED_Lamp也要同时修改
    explicit LED_Lamp (gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_num_, 0);       //初始化为低电平
 
        /* 2. 把 MCP 工具名改成LED灯相关 */
         
        auto& server = McpServer::GetInstance();
        server.AddTool("LED灯.获取开关状态", "返回LED灯的开/关状态",      // 工具名称   , 工具描述    
                       PropertyList(), [this](const PropertyList&) {
 
                           ESP_LOGW(TAG, "获取到了LED灯的当前状态，当前状态为%s", power_ ? "开" : "关");     //日志记录
                           return power_ ? "{\"灯光状态：\":LED灯是开着的！}" : "{\"灯光状态：\":LED灯是关着的！}";       //返回状态
                        
                       });
    
        server.AddTool("LED灯.打开", "打开LED灯",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = true;
                           gpio_set_level(gpio_num_, 1);    //设置为高电平
                           ESP_LOGW(TAG, "已打开LED灯！");   //日志记录
                           return true;     //返回告诉小智执行成功！
                       });
 
        server.AddTool("LED灯.关闭", "关闭LED灯",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = false;
                           gpio_set_level(gpio_num_, 0);    //设置为低电平
                           ESP_LOGW(TAG, "已关闭LED灯！");    //日志记录
                           return true;     //返回告诉小智执行成功！
                       });
    }
};