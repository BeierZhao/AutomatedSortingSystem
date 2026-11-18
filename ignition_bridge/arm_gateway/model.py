"""The OPC UA address space and the commands, as data.

This mirrors docs/development_guide.md sections 7.2 and 7.3, which is the
IO list given to the Ignition developer. tests/test_io_list.py checks the
running server against the tables in the guide.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone

from asyncua import ua

BOOL = ua.VariantType.Boolean
INT32 = ua.VariantType.Int32
DOUBLE = ua.VariantType.Double
STRING = ua.VariantType.String
DATETIME = ua.VariantType.DateTime

BOTH = "both"
ARM4DOF = "arm4dof"
SORTING = "sorting"

# The OPC UA minimum DateTime, which stands for "no time yet". asyncua cannot
# send a null DateTime value.
NO_TIME = datetime(1601, 1, 1, tzinfo=timezone.utc)

INITIAL = {BOOL: False, INT32: 0, DOUBLE: 0.0, STRING: "", DATETIME: NO_TIME}


@dataclass(frozen=True)
class NodeDef:
    path: str
    vtype: ua.VariantType
    writable: bool = False
    profile: str = BOTH

    @property
    def initial(self) -> bool | int | float | str | datetime:
        return INITIAL[self.vtype]


@dataclass(frozen=True)
class Param:
    node: str  # node name under Cmd/<Command>/
    vtype: ua.VariantType
    field: str  # JSON field in the request body


# How the ESP32's answer is turned into a result flag.
REPLY_OPERATION_RESULT = "operation_result"  # {"operation_result": "success"|"invalid"|"rejected"}
REPLY_EMPTY = "empty"  # {}
REPLY_OPERATION_DONE = "operation_done"  # {"operation_done": true|false}


@dataclass(frozen=True)
class CommandDef:
    name: str
    endpoint: str
    params: tuple[Param, ...]
    flags: tuple[str, ...]  # result flag nodes this command has
    reply: str
    motion: bool = False  # True = answered only when motion ends, long timeout
    body_from_json_node: bool = False  # the single String param is the whole body
    profile: str = BOTH

    def path(self, node: str) -> str:
        return f"Cmd/{self.name}/{node}"


COMMANDS: tuple[CommandDef, ...] = (
    CommandDef(
        name="SetArmPosition",
        endpoint="/controller/set-arm-position",
        params=(Param("X", DOUBLE, "x"), Param("Y", DOUBLE, "y"), Param("Z", DOUBLE, "z")),
        flags=("Done", "Invalid", "Rejected", "CommFault"),
        reply=REPLY_OPERATION_RESULT,
        motion=True,
    ),
    CommandDef(
        name="SetJointPosition",
        endpoint="/controller/set-joint-position",
        params=(Param("JointNumber", INT32, "joint_number"), Param("Angle", DOUBLE, "angle")),
        flags=("Done", "Invalid", "Rejected", "CommFault"),
        reply=REPLY_OPERATION_RESULT,
        motion=True,
    ),
    CommandDef(
        name="SetRunningPermit",
        endpoint="/controller/set-running-permit",
        params=(Param("Permit", BOOL, "permit"),),
        flags=("Done", "CommFault"),
        reply=REPLY_EMPTY,
    ),
    CommandDef(
        name="MoveConveyor",
        endpoint="/controller/move-conveyor",
        params=(Param("Direction", BOOL, "direction"), Param("Distance", INT32, "distance")),
        flags=("Done", "Rejected", "CommFault"),
        reply=REPLY_OPERATION_RESULT,
        motion=True,
        profile=SORTING,
    ),
    CommandDef(
        name="UpdatePositionConfig",
        endpoint="/controller/update-position-config",
        params=(Param("ConfigJson", STRING, ""),),
        flags=("Done", "Failed", "CommFault"),
        reply=REPLY_OPERATION_DONE,
        body_from_json_node=True,
        profile=SORTING,
    ),
)

MONITORING_NODES: tuple[NodeDef, ...] = (
    NodeDef("Status/Current", STRING),
    NodeDef("Arm/X", DOUBLE),
    NodeDef("Arm/Y", DOUBLE),
    NodeDef("Arm/Z", DOUBLE),
    NodeDef("Arm/Joint1", DOUBLE),
    NodeDef("Arm/Joint2", DOUBLE),
    NodeDef("Arm/Joint3", DOUBLE),
    NodeDef("Arm/Joint4", DOUBLE),
    NodeDef("Arm/ObstacleDistance", INT32, profile=ARM4DOF),
    # Mapped alarm nodes (for example Alarms/Obstacle) come from the config.
    NodeDef("Alarms/OtherActive", BOOL),
    NodeDef("Alarms/OtherMessage", STRING),
    NodeDef("Comm/EspOnline", BOOL),
    NodeDef("Comm/LastReportTime", DATETIME),
    NodeDef("Comm/Heartbeat", INT32),
    NodeDef("Result/Seq", INT32, profile=SORTING),
    NodeDef("Result/ReadingResult", STRING, profile=SORTING),
    NodeDef("Result/Origin", STRING, profile=SORTING),
    NodeDef("Result/Destination", STRING, profile=SORTING),
    NodeDef("Result/PartId", STRING, profile=SORTING),
    NodeDef("Result/Repository", STRING, profile=SORTING),
    NodeDef("Result/SortingResult", BOOL, profile=SORTING),
)


def in_profile(item_profile: str, profile: str) -> bool:
    return item_profile in (BOTH, profile)


def commands_for(profile: str) -> list[CommandDef]:
    return [c for c in COMMANDS if in_profile(c.profile, profile)]


def nodes_for(profile: str, alarms: dict[str, str]) -> list[NodeDef]:
    """Every variable node of the address space, in browse order."""
    nodes = [n for n in MONITORING_NODES if in_profile(n.profile, profile)]
    for path in dict.fromkeys(alarms.values()):  # unique, order kept
        nodes.append(NodeDef(path, BOOL))
    for cmd in commands_for(profile):
        for p in cmd.params:
            nodes.append(NodeDef(cmd.path(p.node), p.vtype, writable=True))
        nodes.append(NodeDef(cmd.path("Execute"), BOOL, writable=True))
        nodes.append(NodeDef(cmd.path("Busy"), BOOL))
        for flag in cmd.flags:
            nodes.append(NodeDef(cmd.path(flag), BOOL))
    return nodes
