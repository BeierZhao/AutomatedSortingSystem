"""The periodic read: Arm/ nodes, Comm/EspOnline, Comm/Heartbeat."""

import asyncio


async def test_arm_nodes_follow_the_esp32(arm):
    arm.stub.pose = {"x": 180.5, "y": -40, "z": 62.3}
    arm.stub.angles = [-12.5, 67, 32.1, 15]
    arm.stub.distance = 183
    await arm.wait_for("Arm/X", 180.5)
    await arm.wait_for("Arm/Y", -40.0)
    await arm.wait_for("Arm/Z", 62.3)
    await arm.wait_for("Arm/Joint1", -12.5)
    await arm.wait_for("Arm/Joint2", 67.0)
    await arm.wait_for("Arm/Joint3", 32.1)
    await arm.wait_for("Arm/Joint4", 15.0)
    await arm.wait_for("Arm/ObstacleDistance", 183)
    assert await arm.read("Comm/EspOnline") is True

    arm.stub.distance = -1  # nothing in range
    await arm.wait_for("Arm/ObstacleDistance", -1)


async def test_sorting_profile_does_not_read_obstacle_distance(sorting):
    await sorting.wait_for("Comm/EspOnline", True)
    await sorting.wait_for("Arm/X", 150.0)
    await asyncio.sleep(0.3)
    assert sorting.stub.requests_to("get-obstacle-distance") == []
    assert sorting.stub.requests_to("get-current-joint-position")


async def test_esp_online_goes_false_when_stub_is_down_and_recovers(arm):
    await arm.wait_for("Comm/EspOnline", True)
    await arm.wait_for("Arm/X", 150.0)

    await arm.stub.stop()
    await arm.wait_for("Comm/EspOnline", False)
    assert await arm.read("Arm/X") == 150.0  # last value is kept

    await arm.stub.start()
    await arm.wait_for("Comm/EspOnline", True)


async def test_esp_offline_when_reads_time_out(arm):
    await arm.wait_for("Comm/EspOnline", True)
    arm.stub.read_delay = 2.0  # longer than request_timeout_s = 0.5
    await arm.wait_for("Comm/EspOnline", False)


async def test_esp_offline_on_unusable_answer(arm):
    await arm.wait_for("Comm/EspOnline", True)
    arm.stub.angles = [0.0, 67.0]  # too few joints
    await arm.wait_for("Comm/EspOnline", False)
    arm.stub.angles = [0.0, 67.0, 32.0, 0.0]
    await arm.wait_for("Comm/EspOnline", True)
    arm.stub.http_status = 500
    await arm.wait_for("Comm/EspOnline", False)


async def test_heartbeat_increments_even_when_esp_is_down(arm):
    await arm.stub.stop()
    first = await arm.read("Comm/Heartbeat")
    await asyncio.sleep(0.5)
    second = await arm.read("Comm/Heartbeat")
    assert second >= first + 3
