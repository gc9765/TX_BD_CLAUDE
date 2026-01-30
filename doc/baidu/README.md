
# 产物结构
- `include/`：百度大模型实时互动SDK头文件。
- `libs/`：百度大模型实时互动SDK静态库文件。
- `TXW81x_FPV-v2.5.3.7-33742-*`：集成了SDK的泰芯平台工程源码（可直接编译运行验证效果）。
- `RTOS SDK互动方案集成文档`：本地接口说明文档  
  [在线文档参考](https://cloud.baidu.com/doc/RTC/s/Xm8y487ix){target="_blank"}。
- `泰芯平台集成说明`：平台侧修改建议文档。
- `src/`：Demo示例程序。
  - `brtc_wrappe.c`：智能体语音互动Demo（演示完整对话流程）。

# SDK核心函数
1. `baidu_create_chat_agent_engine`：创建智能体引擎。
2. `baidu_chat_agent_engine_init`：初始化参数和事件回调
   - 设置事件回调
   - 配置BRTC房间参数`BDCloudDefaultRTCAppID`（需[申请](https://cloud.baidu.com/doc/RTC/index.html){target="_blank"}）。
3. `baidu_chat_agent_engine_call`：开启智能体对话。
4. `baidu_chat_agent_engine_destroy`：结束对话并销毁引擎。

# 集成注意事项
1. `libbrtc.a`基于泰芯版本`TXW81x_FPV-v2.5.3.7-33742`编译。
2. `TXW81x_FPV-v2.5.3.7-33742-*.rar`适配8M flash板卡，可直接验证。
3. 需结合泰芯平台修改建议优化运行效果（参考集成说明文档）。
4. 泰芯&百库共建代码[仓库地址](https://cloud.baidu.com/doc/RTC/index.html){target="_blank"}（需申请开通）。

# 版本更新说
- v3.1.0
	- 新增视觉理解功能； 	
