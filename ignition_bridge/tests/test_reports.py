"""/scada/... report endpoints and the alarm mapping."""

from datetime import datetime, timedelta, timezone

import pytest
from asyncua import ua

from arm_gateway.model import NO_TIME


def as_utc(value: datetime) -> datetime:
    return value if value.tzinfo else value.replace(tzinfo=timezone.utc)


async def test_report_current_status(arm):
    assert await arm.read("Status/Current") == ""
    assert await arm.report("report-current-status", {"status": "idle"}) == (200, {})
    assert await arm.read("Status/Current") == "idle"
    assert await arm.report("report-current-status", {"status": "manual"}) == (200, {})
    assert await arm.read("Status/Current") == "manual"


async def test_unknown_fields_are_ignored(arm):
    assert await arm.report("report-current-status", {"status": "stop", "extra": 1}) == (200, {})
    assert await arm.read("Status/Current") == "stop"


async def test_last_report_time(arm):
    assert as_utc(await arm.read("Comm/LastReportTime")) == NO_TIME
    before = datetime.now(timezone.utc) - timedelta(seconds=1)
    await arm.report("report-current-status", {"status": "idle"})
    stamp = as_utc(await arm.read("Comm/LastReportTime"))
    assert before <= stamp <= datetime.now(timezone.utc) + timedelta(seconds=1)


async def test_mapped_alarm(arm):
    assert await arm.read("Alarms/Obstacle") is False
    body = {"alarm_message": "Obstacle detected", "alarm_status": True}
    assert await arm.report("report-alarm", body) == (200, {})
    assert await arm.read("Alarms/Obstacle") is True
    # A mapped alarm does not touch the nodes for unmapped ones.
    assert await arm.read("Alarms/OtherActive") is False
    assert await arm.read("Alarms/OtherMessage") == ""

    body["alarm_status"] = False
    assert await arm.report("report-alarm", body) == (200, {})
    assert await arm.read("Alarms/Obstacle") is False


async def test_unmapped_alarm(arm):
    assert await arm.report("report-alarm", {"alarm_message": "Servo overheated", "alarm_status": True}) == (200, {})
    assert await arm.read("Alarms/OtherActive") is True
    assert await arm.read("Alarms/OtherMessage") == "Servo overheated"
    assert await arm.read("Alarms/Obstacle") is False

    # A second unmapped alarm: the message shows the most recent one, and
    # OtherActive stays true until both are reset.
    await arm.report("report-alarm", {"alarm_message": "Vision timeout", "alarm_status": True})
    assert await arm.read("Alarms/OtherMessage") == "Vision timeout"
    await arm.report("report-alarm", {"alarm_message": "Servo overheated", "alarm_status": False})
    assert await arm.read("Alarms/OtherActive") is True
    await arm.report("report-alarm", {"alarm_message": "Vision timeout", "alarm_status": False})
    assert await arm.read("Alarms/OtherActive") is False
    assert await arm.read("Alarms/OtherMessage") == "Vision timeout"


async def test_alarm_table_from_config(envs):
    env = await envs("sorting", alarms={"Gripper jammed": "Alarms/GripperJam"})
    await env.report("report-alarm", {"alarm_message": "Gripper jammed", "alarm_status": True})
    assert await env.read("Alarms/GripperJam") is True
    assert await env.read("Alarms/OtherActive") is False
    # "Obstacle detected" is not in this table, so it is an unmapped alarm
    # and Alarms/Obstacle does not exist.
    await env.report("report-alarm", {"alarm_message": "Obstacle detected", "alarm_status": True})
    assert await env.read("Alarms/OtherMessage") == "Obstacle detected"
    with pytest.raises(ua.UaStatusCodeError):
        await env.read("Alarms/Obstacle")


SORTING_RESULT = {
    "reading_result": "success",
    "origin": "Toronto",
    "destination": "Hamilton",
    "part_id": "10234",
    "repository": "Repo1",
    "sorting_result": True,
}


async def test_report_sorting_result_and_seq(sorting):
    assert await sorting.read("Result/Seq") == 0
    assert await sorting.report("report-sorting-result", SORTING_RESULT) == (200, {})
    assert await sorting.read("Result/Seq") == 1
    assert await sorting.read("Result/ReadingResult") == "success"
    assert await sorting.read("Result/Origin") == "Toronto"
    assert await sorting.read("Result/Destination") == "Hamilton"
    assert await sorting.read("Result/PartId") == "10234"
    assert await sorting.read("Result/Repository") == "Repo1"
    assert await sorting.read("Result/SortingResult") is True

    # An identical result still increments Seq.
    assert await sorting.report("report-sorting-result", SORTING_RESULT) == (200, {})
    assert await sorting.read("Result/Seq") == 2

    failed = dict(SORTING_RESULT, reading_result="info lost", destination="fault", repository="manual", sorting_result=False)
    assert await sorting.report("report-sorting-result", failed) == (200, {})
    assert await sorting.read("Result/Seq") == 3
    assert await sorting.read("Result/Repository") == "manual"
    assert await sorting.read("Result/SortingResult") is False


async def test_bad_sorting_result_does_not_increment_seq(sorting):
    incomplete = {k: v for k, v in SORTING_RESULT.items() if k != "part_id"}
    status, body = await sorting.report("report-sorting-result", incomplete)
    assert status == 400 and "part_id" in body["error"]
    assert await sorting.read("Result/Seq") == 0
    assert await sorting.read("Result/Origin") == ""


async def test_sorting_result_not_served_in_arm4dof(arm):
    status, body = await arm.report("report-sorting-result", SORTING_RESULT)
    assert status == 404 and "error" in body


@pytest.mark.parametrize(
    "endpoint, raw",
    [
        ("report-current-status", b"{not json"),
        ("report-current-status", b""),
        ("report-current-status", b"[1, 2]"),
        ("report-current-status", b"{}"),
        ("report-current-status", b'{"status": 5}'),
        ("report-alarm", b'{"alarm_message": "x"}'),
        ("report-alarm", b'{"alarm_message": "x", "alarm_status": "true"}'),
        ("report-alarm", b'{"alarm_message": "x", "alarm_status": 1}'),
    ],
)
async def test_malformed_report_gets_4xx_with_json_error(arm, endpoint, raw):
    status, body = await arm.report(endpoint, raw=raw)
    assert status == 400
    assert isinstance(body.get("error"), str) and body["error"]
    # Nothing was updated.
    assert await arm.read("Status/Current") == ""
    assert await arm.read("Alarms/OtherActive") is False
    assert as_utc(await arm.read("Comm/LastReportTime")) == NO_TIME


async def test_unknown_path_and_wrong_method_get_json_errors(arm):
    base = f"http://127.0.0.1:{arm.cfg.http_port}"
    async with arm.http.post(f"{base}/scada/nope", json={}) as resp:
        assert resp.status == 404 and "error" in await resp.json()
    async with arm.http.get(f"{base}/scada/report-alarm") as resp:
        assert resp.status == 405 and "error" in await resp.json()
