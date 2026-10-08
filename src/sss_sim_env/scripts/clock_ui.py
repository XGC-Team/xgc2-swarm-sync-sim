#!/usr/bin/env python3
# coding:utf-8
# @author: Peixuan Shu
# @date: 2024-1-19
# @brief: sim clock GUI
# @license: BSD v3

import sys
import os
import signal

from PyQt5.QtWidgets import*
from PyQt5.QtCore import *
from PyQt5.QtGui import QIcon, QPixmap
from ui.main_window import *

import rospy
from rosgraph_msgs.msg import Clock
from xgc2_xrpc.http import Client, Limits
from xgc2_xrpc.runtime import Runtime
import threading
import time


class RosCommNode(QObject):
    signal_clock = pyqtSignal(object)
    signal_dial = pyqtSignal(object)
    def __init__(self, init_speed):
        super(RosCommNode,self).__init__()
        self.expected_speed = init_speed
        self.dial_value = self.speed2dialvalue(self.expected_speed)
        self.clock_sub = rospy.Subscriber("/clock", Clock, self.cb_clock)
        path = rospy.get_param("/sss_clock/control_socket")
        self.runtime = Runtime(blocking_workers=1, max_calls=4, max_connections=4, max_sessions=2)
        discovery = Client(path, runtime=self.runtime, limits=Limits(response_bytes=4096))
        descriptor = discovery.json("/v1/sss-clock/describe", method="GET", timeout=2.0)
        discovery.close()
        self.control = Client(path, runtime=self.runtime, limits=Limits(response_bytes=4096),
                              instance_id=descriptor["instance_id"])
        self.revision = self.control.json("/v1/sss-clock/status", {}, timeout=1.0)["desired_revision"]

    def close(self):
        self.control.close()
        self.runtime.close()

    def apply_control(self, proceed, speed):
        # Revision conflicts and unknown delivery are surfaced. No blind replay.
        result = self.control.json("/v1/sss-clock/control", {
            "expected_revision": self.revision, "proceed": proceed,
            "max_speed_ratio": speed}, timeout=1.0)
        self.revision = result["applied_revision"]
        self.expected_speed = result["max_speed_ratio"]
        return result

    def cb_clock(self,msg):
        now_sim_time = msg.clock.to_sec()
        # print("clock: {}".format(msg.clock.to_sec()))
        try:
            self.signal_clock.emit(now_sim_time)
        except Exception as e:
            # print(e)
            pass

    def call_start(self):
        try:
            self.apply_control(True, self.expected_speed)
        except Exception as error:
            print("[ClockUI] start rejected:", error)

    def call_pause(self):
        try:
            self.apply_control(False, self.expected_speed)
        except Exception as error:
            print("[ClockUI] pause rejected:", error)

    def call_speed_change(self, dial_value):
        if dial_value == self.dial_value:
            return
        try:
            self.apply_control(True, self.dialvalue2speed(dial_value))
            self.dial_value = dial_value
        except Exception as error:
            print("[ClockUI] speed change rejected:", error)
            self.signal_dial.emit(self.dial_value)

    def dialvalue2speed(self, dial_value):
        speed = 0.0
        if dial_value < 10:
            speed = 0.1 + (dial_value - 0) / 10 * (1 - 0.1)
        elif dial_value < 20:
            speed = 1 + (dial_value - 10) / 10 * (2 - 1)
        elif dial_value < 30:
            speed = 2 + (dial_value - 20) / 10 * (10 - 2)
        elif dial_value < 40:
            speed = 10 + (dial_value - 30) / 10 * (20 - 10)
        else:
            speed = 20 + (dial_value - 40) / 10 * (100 - 20)
        return speed

    def speed2dialvalue(self, speed):
        dialvalue = 0
        if speed < 0.1:
            dialvalue = 0
        elif speed < 1:
            dialvalue = 0 + (speed - 0.1) / (1 - 0.1) * 10
        elif speed < 2:
            dialvalue = 10 + (speed - 1) / (2 - 1) * 10
        elif speed < 10:
            dialvalue = 20 + (speed - 2) / (10 - 2) * 10
        elif speed < 20:
            dialvalue = 30 + (speed - 10) / (20 - 10) * 10
        elif speed < 100:
            dialvalue = 40 + (speed - 20) / (100 - 20) * 10
        else:
            dialvalue = 50
        return int(dialvalue)


class MainWindow(QMainWindow,Ui_MainWindow):
    def __init__(self):
        super(MainWindow,self).__init__()
        self.setupUi(self)
    def set_clock_text(self, time):
        self.label_time.setText("{:.4f}".format(time).rstrip('0') + 's')
    def set_dial_value(self, value):
        self.speed_dial.setValue(value)
    def set_progress_bar_value(self, msg):
        value = msg[0]
        text = msg[1]
        self.progressBar.setValue(value)
        self.progressBar.setFormat(text)

class ClockUI(QObject):
    signal_progressbar = pyqtSignal(object)
    def __init__(self, init_speed):
        super(ClockUI,self).__init__()
        self.last_sim_time = 0.0
        self.now_sim_time = 0.0
        self.check_period = 0.2
        self.init_speed = init_speed
    
    def run(self):
        ### Create ros node
        rospy.init_node("clock_ui_pyqt")
        ### Create QApplication window
        self.app = QApplication(sys.argv)
        w = MainWindow()
        # w.resize(400, 200)
        # w.move(300, 300)
        w.setWindowTitle("Sim Clock")

        ### Set logo icon
        workspace = os.path.dirname(os.path.abspath(__file__)) # absolute path of this folder 
        icon = QIcon() # 地面站logo
        print(workspace)
        icon.addPixmap(QPixmap(workspace+"/clock.png"), QIcon.Normal, QIcon.Off)
        w.setWindowIcon(icon)

        w.show()

        self.ros_node = RosCommNode(self.init_speed)

        ### Connect to button signals
        w.startButton.clicked.connect(lambda:self.ros_node.call_start())
        w.pauseButton.clicked.connect(lambda:self.ros_node.call_pause())
        w.speed_dial.valueChanged.connect(lambda:self.ros_node.call_speed_change(w.speed_dial.value()))

        ### Connect to subscriber signals
        self.ros_node.signal_clock.connect(w.set_clock_text)
        self.ros_node.signal_clock.connect(self.update_sim_time)
        self.ros_node.signal_dial.connect(w.set_dial_value)
        self.signal_progressbar.connect(w.set_progress_bar_value)

        self.ros_node.signal_dial.emit(self.ros_node.dial_value)

        print("Sim Clock UI")
        check_speed_thread= threading.Thread(target = self.check_speed)
        check_speed_thread.setDaemon(True) # kill thread when app exits
        check_speed_thread.start()

        # Block the main funcaton. Release resourses.
        signal.signal(signal.SIGINT, self._signal_handler) # catch Ctrl+C or terminate()
        code = self.app.exec_()
        self.ros_node.close()
        sys.exit(code)
    
    def _signal_handler(self, signal, frame):
        sys.exit(0)

    
    def update_sim_time(self, time):
        self.now_sim_time = time

    def check_speed(self):
        while (not rospy.is_shutdown()):
            speed = (self.now_sim_time - self.last_sim_time) / self.check_period
            expected_speed = self.ros_node.expected_speed
            speed_perct = int(speed/expected_speed * 100)
            speed_perct = max(min(speed_perct, 100), 0)
            # print("speed_pertc: {}".format(speed_perct))
            try:
                self.signal_progressbar.emit([speed_perct, "{:.2f}x".format(speed)])
            except Exception as e:
                # print(e)
                pass
            self.last_sim_time = self.now_sim_time
            time.sleep(self.check_period)

if __name__ == '__main__':
    if len(sys.argv) > 1:
        init_speed = float(sys.argv[1])
    else:
        init_speed = 1.0
    clock_ui = ClockUI(init_speed)
    clock_ui.run()
