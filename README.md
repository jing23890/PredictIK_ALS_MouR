# PredictIK_ALS_MouR
基于ALS框架的预测ik制作尝试

## 当前版本

UE 5.7 项目，使用 `ALS_AnimBP` 的 `PredictIK` 层，核心计算位于 `UPIKAnimInstance`。

- 当前仅验证前进走路 `ALS_N_Walk_F`，不启用站立/其他方向预测，也未接入常规 IK 回退。
- 左右脚独立维护支撑锚点、预测落点和双向障碍检测路径。
- 保留 14 cm 落地容差，离地阈值为 14.5 cm。
- 盆骨采用世界高度 pivot 插值，双脚使用组件空间目标解算。
- 已开启单帧 Trace、路径线和 Debug Box；可在动画蓝图类默认值中关闭 `bPIK_DrawDebug`。
- `PIK_PelvisInterpSpeed` 控制高度跟随速度，当前默认值为 10。

传统贴地与预测的逐脚交接方案目前仅讨论，尚未实现。

## 获取与打开

安装 Git LFS 后克隆仓库，并执行 `git lfs pull` 获取动画及地图资产。
使用 UE 5.7 打开 `AdvancedLocomotionV4.uproject`。首次打开需要本机 C++ 构建环境，并编译项目模块和随项目提供的 MCP 插件源码。
编译产物、缓存及本机配置不纳入版本控制。

## 验证

- C++ 与动画蓝图编译通过。
- `ALS.PredictIK.PathGeometry` 自动测试覆盖平地、上下台阶、障碍路径及无效路径。
- 完成平地前进走路与临时 20 cm 台阶运行采样。
- `Scripts/PIK_IsolatedProbe.py` 仅用于带 `-PIKIsolatedTest` 标记的独立测试进程，不保存测试场景。
