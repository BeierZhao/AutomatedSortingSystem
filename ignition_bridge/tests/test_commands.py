"""The command handshake of guide section 7.4, for every command."""

import asyncio
import json

import pytest
from asyncua import ua

from conftest import only

ARM_TARGET = dict(X=150.0, Y=-20.5, Z=50.0)
JOINT_TARGET = dict(JointNumber=4, Angle=30.0)

# command, parameters, expected endpoint, expected request body
MOTION = [
    ("SetArmPosition", ARM_TARGET, "set-arm-position", {"x": 150.0, "y": -20.5, "z": 50.0}),
    ("SetJointPosition", JOINT_TARGET, "set-joint-position", {"joint_number": 4, "angle": 30.0}),
]


@pytest.mark.parametrize("command, params, endpoint, body", MOTION)
@pytest.mark.parametrize("result, flag", [("success", "Done"), ("invalid", "Invalid"), ("rejected", "Rejected")])
async def test_motion_command_results(arm, command, params, endpoint, body, result, flag):
    arm.stub.results[endpoint] = result
    assert only(await arm.execute(command, **params)) == flag
    assert arm.stub.requests_to(endpoint) == [body]
    # joint_number must be a JSON integer for the firmware's is<int>() check.
    if "joint_number" in body:
        assert type(arm.stub.requests_to(endpoint)[0]["joint_number"]) is int


@pytest.mark.parametrize("command, params, endpoint, body", MOTION)
async def test_motion_command_comm_fault_when_esp_down(arm, command, params, endpoint, body):
    await arm.stub.stop()
    assert only(await arm.execute(command, **params)) == "CommFault"


async def test_comm_fault_on_timeout(arm):
    arm.stub.motion_delay = 3.0  # longer than motion_timeout_s = 1.0
    assert only(await arm.execute("SetArmPosition", **ARM_TARGET)) == "CommFault"


@pytest.mark.parametrize("status", [400, 500])
async def test_comm_fault_on_non_200(arm, status):
    arm.stub.http_status = status
    assert only(await arm.execute("SetArmPosition", **ARM_TARGET)) == "CommFault"
    assert only(await arm.execute("SetRunningPermit", Permit=True)) == "CommFault"


@pytest.mark.parametrize("permit", [True, False])
async def test_set_running_permit(arm, permit):
    flags = await arm.execute("SetRunningPermit", Permit=permit)
    assert set(flags) == {"Done", "CommFault"}  # this command has no other flags
    assert only(flags) == "Done"
    assert arm.stub.requests_to("set-running-permit") == [{"permit": permit}]
    assert arm.stub.permit is permit


async def test_set_running_permit_comm_fault(arm):
    await arm.stub.stop()
    assert only(await arm.execute("SetRunningPermit", Permit=True)) == "CommFault"


async def test_busy_during_motion_and_execute_ignored_while_busy(arm):
    arm.stub.motion_delay = 0.5
    base = "Cmd/SetArmPosition/"
    for name, value in ARM_TARGET.items():
        await arm.write(base + name, value)
    await arm.write(base + "Execute", True)
    await arm.wait_for(base + "Busy", True)
    assert not any((await arm.flags("SetArmPosition")).values())

    # A second Execute while Busy is ignored: no second request.
    await arm.write(base + "X", 99.0)
    await arm.write(base + "Execute", True)
    await asyncio.sleep(0.1)
    assert await arm.read(base + "Busy") is True

    await arm.wait_for(base + "Busy", False)
    await arm.wait_for(base + "Execute", False)
    assert only(await arm.flags("SetArmPosition")) == "Done"
    await asyncio.sleep(0.2)
    assert len(arm.stub.requests_to("set-arm-position")) == 1
    assert arm.stub.requests_to("set-arm-position")[0]["x"] == 150.0


async def test_flags_latch_until_next_execute(arm):
    arm.stub.results["set-arm-position"] = "invalid"
    assert only(await arm.execute("SetArmPosition", **ARM_TARGET)) == "Invalid"
    await asyncio.sleep(0.3)  # several poll cycles later
    assert only(await arm.flags("SetArmPosition")) == "Invalid"

    # Another command does not touch these flags.
    assert only(await arm.execute("SetRunningPermit", Permit=True)) == "Done"
    assert only(await arm.flags("SetArmPosition")) == "Invalid"

    # The next Execute clears the old flag while the command is running.
    arm.stub.results["set-arm-position"] = "success"
    arm.stub.motion_delay = 0.4
    await arm.write("Cmd/SetArmPosition/Execute", True)
    await arm.wait_for("Cmd/SetArmPosition/Busy", True)
    assert not any((await arm.flags("SetArmPosition")).values())
    await arm.wait_for("Cmd/SetArmPosition/Execute", False)
    assert only(await arm.flags("SetArmPosition")) == "Done"


async def test_writing_execute_false_does_nothing(arm):
    await arm.write("Cmd/SetArmPosition/Execute", False)
    await asyncio.sleep(0.2)
    assert arm.stub.requests_to("set-arm-position") == []
    assert await arm.read("Cmd/SetArmPosition/Busy") is False


async def test_second_motion_command_while_first_is_running(arm):
    """Guide 7.4: allowed at the OPC UA level, the ESP32 rejects the second."""
    arm.stub.results = {name: "auto" for name in arm.stub.results}
    arm.stub.permit = True
    arm.stub.motion_delay = 0.5
    for name, value in ARM_TARGET.items():
        await arm.write(f"Cmd/SetArmPosition/{name}", value)
    await arm.write("Cmd/SetArmPosition/Execute", True)
    await arm.wait_for("Cmd/SetArmPosition/Busy", True)

    assert only(await arm.execute("SetJointPosition", **JOINT_TARGET)) == "Rejected"
    assert await arm.read("Cmd/SetArmPosition/Busy") is True
    await arm.wait_for("Cmd/SetArmPosition/Execute", False)
    assert only(await arm.flags("SetArmPosition")) == "Done"


async def test_polling_continues_during_motion(arm):
    arm.stub.motion_delay = 0.6
    await arm.write("Cmd/SetArmPosition/Execute", True)
    await arm.wait_for("Cmd/SetArmPosition/Busy", True)
    arm.stub.distance = 77
    await arm.wait_for("Arm/ObstacleDistance", 77, timeout=0.5)
    assert await arm.read("Cmd/SetArmPosition/Busy") is True
    await arm.wait_for("Cmd/SetArmPosition/Execute", False)


async def test_unexpected_answer_is_comm_fault(arm):
    arm.stub.results["set-arm-position"] = "auto"
    arm.stub._motion = None  # make the handler crash -> HTTP 500
    assert only(await arm.execute("SetArmPosition", **ARM_TARGET)) == "CommFault"


async def test_client_cannot_write_wrong_type(arm):
    with pytest.raises(ua.UaStatusCodeError):
        await arm.node("Cmd/SetArmPosition/X").write_value(ua.DataValue(ua.Variant("abc", ua.VariantType.String)))


# ---------------------------------------------------------------- sorting profile


@pytest.mark.parametrize("result, flag", [("success", "Done"), ("rejected", "Rejected")])
async def test_move_conveyor(sorting, result, flag):
    sorting.stub.results["move-conveyor"] = result
    flags = await sorting.execute("MoveConveyor", Direction=True, Distance=100)
    assert set(flags) == {"Done", "Rejected", "CommFault"}
    assert only(flags) == flag
    assert sorting.stub.requests_to("move-conveyor") == [{"direction": True, "distance": 100}]


async def test_move_conveyor_comm_fault(sorting):
    await sorting.stub.stop()
    assert only(await sorting.execute("MoveConveyor", Direction=False, Distance=50)) == "CommFault"


async def test_move_conveyor_invalid_answer_is_comm_fault(sorting):
    # MoveConveyor has no Invalid flag, so "invalid" is an unexpected answer.
    sorting.stub.results["move-conveyor"] = "invalid"
    assert only(await sorting.execute("MoveConveyor", Direction=True, Distance=100)) == "CommFault"


POSITION_CONFIG = {
    "repositories": {"Repo1": {"x": 50, "y": 50, "z": 0}, "Repo2": {"x": 100, "y": 50, "z": 0}},
    "destinations": {"Hamilton": "Repo1", "Burlington": "Repo1", "Mississauga": "Repo2"},
    "part_dock": {"x": 10, "y": 10, "z": 10},
}


async def test_update_position_config_done(sorting):
    flags = await sorting.execute("UpdatePositionConfig", ConfigJson=json.dumps(POSITION_CONFIG))
    assert set(flags) == {"Done", "Failed", "CommFault"}
    assert only(flags) == "Done"
    assert sorting.stub.position_config == POSITION_CONFIG


async def test_update_position_config_failed(sorting):
    sorting.stub.operation_done = False
    assert only(await sorting.execute("UpdatePositionConfig", ConfigJson=json.dumps(POSITION_CONFIG))) == "Failed"


@pytest.mark.parametrize("text", ["", "{not json", "[1, 2]"])
async def test_update_position_config_bad_json_is_failed_and_not_sent(sorting, text):
    assert only(await sorting.execute("UpdatePositionConfig", ConfigJson=text)) == "Failed"
    assert sorting.stub.requests_to("update-position-config") == []


async def test_update_position_config_comm_fault(sorting):
    await sorting.stub.stop()
    assert only(await sorting.execute("UpdatePositionConfig", ConfigJson=json.dumps(POSITION_CONFIG))) == "CommFault"


async def test_arm_commands_work_in_sorting_profile(sorting):
    assert only(await sorting.execute("SetArmPosition", **ARM_TARGET)) == "Done"
    assert only(await sorting.execute("SetRunningPermit", Permit=True)) == "Done"
