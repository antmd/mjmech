# Copyright 2014 Josh Pieper, jjp@pobox.com.  All rights reserved.

import eventlet
import functools

import PySide.QtCore as QtCore
import PySide.QtGui as QtGui

from mtool_common import BoolContext
import servo_controller

def spawn(callback):
    def task():
        eventlet.spawn(callback)

    return task

class ServoTab(object):
    def __init__(self, ui):
        self.ui = ui

        self.servo_controls = []

        self.ui.statusText.setText('not connected')
        self.ui.connectButton.clicked.connect(
            spawn(self.handle_connect_clicked))

        servo_layout = QtGui.QVBoxLayout()
        servo_layout.setSpacing(0)
        servo_layout.setContentsMargins(0, 0, 0, 0)
        self.ui.scrollContents.setLayout(servo_layout)

        self.ui.servoCountSpin.valueChanged.connect(self.handle_servo_count)
        self.handle_servo_count()

        self.ui.powerCombo.currentIndexChanged.connect(
            spawn(self.handle_power))

        self.ui.captureCurrentButton.clicked.connect(
            spawn(self.handle_capture_current))

        self.update_connected(False)

        self.ui.addPoseButton.clicked.connect(self.handle_add_pose)
        self.ui.removePoseButton.clicked.connect(self.handle_remove_pose)
        self.ui.moveToPoseButton.clicked.connect(
            spawn(self.handle_move_to_pose))
        self.ui.updatePoseButton.clicked.connect(self.handle_update_pose)

        self.controller = None
        self.servo_update = BoolContext()

    def resizeEvent(self, event):
        pass

    def poses(self):
        result = []
        for i in range(self.ui.poseList.count()):
            result.append(self.ui.poseList.item(i).text())
        return result

    def pose(self, name):
        for i in range(self.ui.poseList.count()):
            if self.ui.poseList.item(i).text() == name:
                return self.ui.poseList.item(i).data(QtCore.Qt.UserRole)
        return dict([(i, 0.0) for i in range(self.ui.servoCountSpin.value())])

    def handle_connect_clicked(self):
        val = self.ui.typeCombo.currentText().lower()
        self.controller = servo_controller.servo_controller(
            val,
            serial_port=self.ui.serialPortCombo.currentText(),
            model_name=self.ui.modelEdit.text())
        self.ui.statusText.setText('connected')
        self.update_connected(True)

    def handle_servo_count(self):
        count = self.ui.servoCountSpin.value()
        while len(self.servo_controls) > count:
            # Remove the last one
            last = self.servo_controls[-1]
            widget = last['widget']
            self.ui.scrollContents.layout().removeWidget(widget)
            widget.deleteLater()
            self.servo_controls = self.servo_controls[:-1]

        while len(self.servo_controls) < count:
            # Add a new one.

            servo_id = len(self.servo_controls)

            label = QtGui.QLabel()
            label.setText('ID %d:' % servo_id)

            slider = QtGui.QSlider(QtCore.Qt.Horizontal)
            slider.setRange(-180, 180)

            doublespin = QtGui.QDoubleSpinBox()
            doublespin.setRange(-180, 180)
            doublespin.setDecimals(1)

            widget = QtGui.QWidget()

            layout = QtGui.QHBoxLayout(widget)
            layout.addWidget(label)
            layout.addWidget(slider)
            layout.addWidget(doublespin)

            slider.valueChanged.connect(
                functools.partial(self.handle_servo_slider, servo_id))
            doublespin.valueChanged.connect(
                functools.partial(self.handle_servo_spin, servo_id))

            self.ui.scrollContents.layout().addWidget(widget)

            self.servo_controls.append({
                    'widget': widget,
                    'label': label,
                    'slider': slider,
                    'doublespin': doublespin})

    def handle_power(self):
        text = self.ui.powerCombo.currentText().lower()
        value = None
        if text == 'free':
            value = servo_controller.POWER_FREE
        elif text == 'brake':
            value = servo_controller.POWER_BRAKE
        elif text == 'drive':
            value = servo_controller.POWER_ENABLE
        else:
            raise NotImplementedError()

        self.controller.enable_power(value)

    def update_connected(self, value):
        self.ui.controlGroup.setEnabled(value)
        self.ui.posesGroup.setEnabled(value)
        if value:
            self.handle_power()

    def set_single_pose(self, servo_id, value):
        self.controller.set_single_pose(servo_id, value, pose_time=0.2)

    def handle_servo_slider(self, servo_id, event):
        if self.servo_update.value:
            return

        with self.servo_update:
            control = self.servo_controls[servo_id]
            value = control['slider'].value()
            control['doublespin'].setValue(value)
            eventlet.spawn(self.set_single_pose, servo_id, value)

    def handle_servo_spin(self, servo_id, event):
        if self.servo_update.value:
            return

        with self.servo_update:
            control = self.servo_controls[servo_id]
            value = control['doublespin'].value()
            control['slider'].setSliderPosition(int(value))
            eventlet.spawn(self.set_single_pose, servo_id, value)

    def handle_capture_current(self):
        with self.servo_update:
            results = self.controller.get_pose(range(len(self.servo_controls)))
            for ident, angle in results.iteritems():
                control = self.servo_controls[ident]
                control['slider'].setSliderPosition(int(angle))
                control['doublespin'].setValue(angle)

    def add_list_pose(self, name):
        self.ui.poseList.addItem(name)
        item = self.ui.poseList.item(self.ui.poseList.count() - 1)
        item.setFlags(QtCore.Qt.ItemIsEnabled |
                      QtCore.Qt.ItemIsSelectable |
                      QtCore.Qt.ItemIsEditable |
                      QtCore.Qt.ItemIsSelectable)
        return item

    def get_new_pose_name(self):
        poses = set([self.ui.poseList.item(x).text()
                     for x in range(self.ui.poseList.count())])
        count = 0
        while True:
            name = 'new_pose_%d' % count
            if name not in poses:
                return name
            count += 1

    def generate_pose_data(self):
        return dict(
            [ (i, control['doublespin'].value())
              for i, control in enumerate(self.servo_controls) ])

    def handle_add_pose(self):
        pose_name = self.get_new_pose_name()
        item = self.add_list_pose(pose_name)
        item.setData(QtCore.Qt.UserRole, self.generate_pose_data())
        self.ui.poseList.editItem(item)

    def handle_remove_pose(self):
        if self.ui.poseList.currentRow() < 0:
            return
        pose_name = self.ui.poseList.currentItem().text()
        del self.poses[pose_name]
        self.ui.poseList.takeItem(self.ui.poseList.currentRow())

    def handle_move_to_pose(self):
        if self.ui.poseList.currentRow() < 0:
            return

        values = self.ui.poseList.currentItem().data(QtCore.Qt.UserRole)
        self.controller.set_pose(values, pose_time=1.0)
        with self.servo_update:
            for ident, angle_deg in values.iteritems():
                control = self.servo_controls[ident]
                control['slider'].setSliderPosition(int(angle_deg))
                control['doublespin'].setValue(angle_deg)

    def handle_update_pose(self):
        if self.ui.poseList.currentRow() < 0:
            return

        self.ui.poseList.currentItem().setData(
            QtCore.Qt.UserRole, self.generate_pose_data())

    def read_settings(self, config):
        if not config.has_section('servo'):
            return

        self.ui.typeCombo.setCurrentIndex(config.getint('servo', 'type'))
        self.ui.serialPortCombo.setEditText(config.get('servo', 'port'))
        self.ui.modelEdit.setText(config.get('servo', 'model'))
        self.ui.servoCountSpin.setValue(config.getint('servo', 'count'))

        if not config.has_section('servo.poses'):
            return

        for name, value in config.items('servo.poses'):
            this_data = {}
            for element in value.split(','):
                ident, angle_deg = element.split('=')
                this_data[int(ident)] = float(angle_deg)
            item = self.add_list_pose(name)
            item.setData(QtCore.Qt.UserRole, this_data)


    def write_settings(self, config):
        config.add_section('servo')
        config.add_section('servo.poses')

        config.set('servo', 'type', self.ui.typeCombo.currentIndex())
        config.set('servo', 'port', self.ui.serialPortCombo.currentText())
        config.set('servo', 'model', self.ui.modelEdit.text())
        config.set('servo', 'count', self.ui.servoCountSpin.value())

        for row in range(self.ui.poseList.count()):
            item = self.ui.poseList.item(row)
            pose_name = item.text()
            values = item.data(QtCore.Qt.UserRole)
            config.set(
                'servo.poses', pose_name,
                ','.join(['%d=%.2f' % (ident, angle_deg)
                          for ident, angle_deg in values.iteritems()]))
