#!/usr/bin/env python3
"""
定时拍照装置
支持按固定时间间隔拍照，可配置保存路径、图片格式等
"""

import cv2
import os
import time
from datetime import datetime
from pathlib import Path
import argparse
import signal
import sys


class TimelapseCamera:
    def __init__(self, save_dir="photos", interval=60, camera_id=0, 
                 image_format="jpg", quality=95, resolution=None):
        """
        初始化定时拍照相机
        
        Args:
            save_dir: 照片保存目录
            interval: 拍照间隔（秒）
            camera_id: 摄像头ID，默认0为主摄像头
            image_format: 图片格式（jpg/png）
            quality: JPEG质量（1-100）
            resolution: 分辨率元组 (宽, 高)，None则使用默认
        """
        self.save_dir = Path(save_dir)
        self.interval = interval
        self.camera_id = camera_id
        self.image_format = image_format.lower()
        self.quality = quality
        self.resolution = resolution
        self.cap = None
        self.running = False
        self.photo_count = 0
        
        # 创建保存目录
        self.save_dir.mkdir(parents=True, exist_ok=True)
        
        # 设置信号处理，支持优雅退出
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)
    
    def _signal_handler(self, signum, frame):
        """处理退出信号"""
        print("\n接收到退出信号，正在关闭相机...")
        self.running = False
    
    def _init_camera(self):
        """初始化摄像头"""
        print(f"正在初始化摄像头 {self.camera_id}...")
        self.cap = cv2.VideoCapture(self.camera_id)
        
        if not self.cap.isOpened():
            raise RuntimeError(f"无法打开摄像头 {self.camera_id}")
        
        # 设置分辨率
        if self.resolution:
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.resolution[0])
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.resolution[1])
        
        # 等待摄像头预热
        time.sleep(2)
        
        # 读取一帧测试
        ret, frame = self.cap.read()
        if not ret:
            raise RuntimeError("无法从摄像头读取图像")
        
        actual_width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        print(f"摄像头已就绪，分辨率: {actual_width}x{actual_height}")
        
        return True
    
    def _capture_photo(self):
        """拍摄一张照片"""
        ret, frame = self.cap.read()
        if not ret:
            print("警告: 拍照失败")
            return None
        
        # 生成文件名：年月日_时分秒_毫秒
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        filename = f"photo_{timestamp}.{self.image_format}"
        filepath = self.save_dir / filename
        
        # 保存图片
        if self.image_format == "jpg" or self.image_format == "jpeg":
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.quality]
            cv2.imwrite(str(filepath), frame, encode_params)
        else:
            cv2.imwrite(str(filepath), frame)
        
        self.photo_count += 1
        print(f"[{self.photo_count}] 已保存: {filepath}")
        
        return filepath
    
    def _release_camera(self):
        """释放摄像头资源"""
        if self.cap:
            self.cap.release()
            print("摄像头已释放")
    
    def start(self):
        """开始定时拍照"""
        try:
            self._init_camera()
            self.running = True
            
            print(f"\n定时拍照已启动")
            print(f"保存目录: {self.save_dir.absolute()}")
            print(f"拍照间隔: {self.interval} 秒")
            print(f"按 Ctrl+C 停止\n")
            
            # 立即拍第一张
            self._capture_photo()
            
            # 定时拍照循环
            while self.running:
                # 计算下一次拍照时间
                next_capture = time.time() + self.interval
                
                # 等待直到下一次拍照
                while time.time() < next_capture and self.running:
                    time.sleep(0.1)
                
                if self.running:
                    self._capture_photo()
                    
        except Exception as e:
            print(f"错误: {e}")
        finally:
            self._release_camera()
            print(f"\n总共拍摄了 {self.photo_count} 张照片")
            print(f"照片保存在: {self.save_dir.absolute()}")


def main():
    parser = argparse.ArgumentParser(description="定时拍照装置")
    parser.add_argument("-d", "--directory", default="photos",
                        help="照片保存目录 (默认: photos)")
    parser.add_argument("-i", "--interval", type=int, default=60,
                        help="拍照间隔秒数 (默认: 60)")
    parser.add_argument("-c", "--camera", type=int, default=0,
                        help="摄像头ID (默认: 0)")
    parser.add_argument("-f", "--format", default="jpg", choices=["jpg", "png"],
                        help="图片格式 (默认: jpg)")
    parser.add_argument("-q", "--quality", type=int, default=95,
                        help="JPEG质量 1-100 (默认: 95)")
    parser.add_argument("--width", type=int, default=None,
                        help="图片宽度")
    parser.add_argument("--height", type=int, default=None,
                        help="图片高度")
    
    args = parser.parse_args()
    
    # 设置分辨率
    resolution = None
    if args.width and args.height:
        resolution = (args.width, args.height)
    
    # 创建相机实例并启动
    camera = TimelapseCamera(
        save_dir=args.directory,
        interval=args.interval,
        camera_id=args.camera,
        image_format=args.format,
        quality=args.quality,
        resolution=resolution
    )
    
    camera.start()


if __name__ == "__main__":
    main()
