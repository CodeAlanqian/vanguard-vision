# Vanguard_vision

## Start

环境 ROS2-galactic

```shell
#安装相机管理工具
sudo apt install ros-galactic-camera-info-manager

#创建工作空间
mkdir -p ~/ws/src

#下载包
cd ~/ws
git clone https://github.com/CodeAlanqian/vanguard-vision.git ./src

#编译
colcon build --packages-up-to rm_auto_aim
colcon build --packages-select hik_camera

#运行相机发布节点
ros2 launch hik_camera hik_camera.launch.py

#运行自瞄节点
ros2 launch src/rm_auto_aim/auto_aim_bringup/launch/auto_aim.launch.py
```


