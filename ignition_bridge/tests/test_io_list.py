"""The address space must match the IO list in the development guide
(sections 7.2 and 7.3), which was given to the Ignition developer."""

import re
from pathlib import Path

import pytest
from asyncua import ua

from arm_gateway.config import Config, ConfigError, load_config

GUIDE = Path(__file__).resolve().parents[2] / "docs" / "development_guide.md"
EXAMPLE_CONFIG = Path(__file__).resolve().parents[1] / "config.example.toml"

TYPE_IDS = {
    "Boolean": ua.ObjectIds.Boolean,
    "Int32": ua.ObjectIds.Int32,
    "Double": ua.ObjectIds.Double,
    "String": ua.ObjectIds.String,
    "DateTime": ua.ObjectIds.DateTime,
}
PROJECTS = {"Both": ("arm4dof", "sorting"), "4-DOF": ("arm4dof",), "Sorting": ("sorting",)}

ROW = re.compile(r"^\| `([^`]+)` \| (\w+) \| (RW|R) \|.*\| ([\w-]+) \|\s*$")


def guide_io_list(profile: str) -> dict[str, tuple[str, str]]:
    """{node path: (type, access)} for one profile, parsed from the guide."""
    nodes = {}
    for line in GUIDE.read_text(encoding="utf-8").splitlines():
        m = ROW.match(line)
        if m and m.group(2) in TYPE_IDS:
            path, type_name, access, project = m.groups()
            if profile in PROJECTS[project]:
                assert path not in nodes, f"{path} is listed twice in the guide"
                nodes[path] = (type_name, access)
    return nodes


def test_guide_tables_were_parsed():
    if not GUIDE.exists():
        pytest.skip("development guide not found")
    assert len(guide_io_list("arm4dof")) == 37
    assert len(guide_io_list("sorting")) == 55


@pytest.mark.parametrize("profile", ["arm4dof", "sorting"])
async def test_address_space_matches_guide(envs, profile):
    if not GUIDE.exists():
        pytest.skip("development guide not found")
    env = await envs(profile)
    expected = guide_io_list(profile)
    if profile == "sorting":
        # Alarms/Obstacle is 4-DOF only, and the default sorting alarm table is empty.
        assert "Alarms/Obstacle" not in expected

    # Same set of nodes: nothing missing, nothing extra.
    assert set(env.gateway._node_ids) == set(expected)

    for path, (type_name, access) in expected.items():
        node = env.node(path)
        data_type = await node.read_data_type()
        assert data_type == ua.NodeId(TYPE_IDS[type_name], 0), f"{path}: wrong data type"
        value = await node.read_data_value()
        assert value.Value.VariantType.name == type_name, f"{path}: wrong value type"
        levels = await node.get_access_level()
        assert ua.AccessLevel.CurrentRead in levels, f"{path}: not readable"
        assert (ua.AccessLevel.CurrentWrite in levels) == (access == "RW"), f"{path}: wrong access"


async def test_namespace_and_node_id_form(arm):
    namespaces = await arm.client.get_namespace_array()
    assert namespaces.index("urn:group12:arm-gateway") == 2  # guide 7.1: ns=2
    assert isinstance(await arm.client.get_node("ns=2;s=Arm/X").read_value(), float)
    assert (await arm.client.get_node("ns=2;s=Status/Current").read_browse_name()).Name == "Current"


async def test_read_only_nodes_refuse_client_writes(arm):
    for path, value in [("Arm/X", 1.0), ("Status/Current", "idle"), ("Cmd/SetArmPosition/Done", True), ("Comm/Heartbeat", 5)]:
        with pytest.raises(ua.UaStatusCodeError):
            await arm.write(path, value)


async def test_nodes_can_be_browsed_from_objects(arm):
    root = await arm.client.nodes.objects.get_child([f"{arm.ns}:ArmGateway"])
    folders = {(await n.read_browse_name()).Name for n in await root.get_children()}
    assert folders == {"Status", "Arm", "Alarms", "Comm", "Cmd"}
    execute = await root.get_child([f"{arm.ns}:Cmd", f"{arm.ns}:SetArmPosition", f"{arm.ns}:Execute"])
    assert execute.nodeid == ua.NodeId("Cmd/SetArmPosition/Execute", arm.ns)


async def test_sorting_only_nodes_absent_in_arm4dof(arm):
    for path in ("Result/Seq", "Cmd/MoveConveyor/Execute", "Cmd/UpdatePositionConfig/ConfigJson"):
        with pytest.raises(ua.UaStatusCodeError):
            await arm.read(path)


async def test_obstacle_distance_absent_in_sorting(sorting):
    with pytest.raises(ua.UaStatusCodeError):
        await sorting.read("Arm/ObstacleDistance")
    assert await sorting.read("Result/Seq") == 0


def test_example_config_loads_with_guide_defaults():
    cfg = load_config(EXAMPLE_CONFIG)
    assert cfg.profile == "arm4dof"
    assert (cfg.http_host, cfg.http_port) == ("0.0.0.0", 8080)
    assert cfg.opcua_endpoint == "opc.tcp://127.0.0.1:4840/arm-gateway"
    assert cfg.opcua_namespace_uri == "urn:group12:arm-gateway"
    assert (cfg.poll_interval_s, cfg.motion_timeout_s, cfg.request_timeout_s) == (1.0, 30.0, 2.0)
    assert cfg.alarms == {"Obstacle detected": "Alarms/Obstacle"}


def test_config_validation(tmp_path):
    with pytest.raises(ConfigError):
        Config(profile="other")
    with pytest.raises(ConfigError):
        Config(alarms={"x": "Alarms/OtherActive"})
    with pytest.raises(ConfigError):
        Config(alarms={"x": "Status/Current"})
    bad = tmp_path / "bad.toml"
    bad.write_text('[esp32]\nbase_ulr = "http://x"\n', encoding="utf-8")
    with pytest.raises(ConfigError):
        load_config(bad)
