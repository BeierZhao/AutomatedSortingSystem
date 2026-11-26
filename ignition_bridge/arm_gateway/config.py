"""Gateway settings, loaded from a TOML file."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

PROFILES = ("arm4dof", "sorting")

# Used when the config file has no [alarms] table.
DEFAULT_ALARMS = {
    "arm4dof": {"Obstacle detected": "Alarms/Obstacle"},
    "sorting": {},
}

RESERVED_ALARM_NODES = ("Alarms/OtherActive", "Alarms/OtherMessage")


class ConfigError(ValueError):
    pass


@dataclass
class Config:
    profile: str = "arm4dof"
    esp32_base_url: str = "http://192.168.1.50:80"
    poll_interval_s: float = 1.0
    motion_timeout_s: float = 30.0
    request_timeout_s: float = 2.0
    http_host: str = "0.0.0.0"
    http_port: int = 8080
    opcua_endpoint: str = "opc.tcp://127.0.0.1:4840/arm-gateway"
    opcua_namespace_uri: str = "urn:group12:arm-gateway"
    # alarm_message text -> Boolean node path. None = profile default.
    alarms: dict[str, str] | None = None
    log_level: str = "INFO"
    log_file: str | None = None
    # Not in the file: period of Comm/Heartbeat. Tests shorten it.
    heartbeat_interval_s: float = field(default=1.0, repr=False)

    def __post_init__(self) -> None:
        if self.profile not in PROFILES:
            raise ConfigError(f"profile must be one of {PROFILES}, got {self.profile!r}")
        if self.alarms is None:
            self.alarms = dict(DEFAULT_ALARMS[self.profile])
        for message, node in self.alarms.items():
            if not isinstance(node, str) or not node.startswith("Alarms/") or node == "Alarms/":
                raise ConfigError(f'alarm node for {message!r} must look like "Alarms/<Name>"')
            if node in RESERVED_ALARM_NODES:
                raise ConfigError(f"{node} is reserved for unmapped alarms")
        for name in ("poll_interval_s", "motion_timeout_s", "request_timeout_s"):
            if getattr(self, name) <= 0:
                raise ConfigError(f"{name} must be greater than 0")
        self.esp32_base_url = self.esp32_base_url.rstrip("/")


# TOML table -> {TOML key: Config attribute}
_SCHEMA = {
    "esp32": {
        "base_url": "esp32_base_url",
        "poll_interval_s": "poll_interval_s",
        "motion_timeout_s": "motion_timeout_s",
        "request_timeout_s": "request_timeout_s",
    },
    "http": {"host": "http_host", "port": "http_port"},
    "opcua": {"endpoint": "opcua_endpoint", "namespace_uri": "opcua_namespace_uri"},
    "logging": {"level": "log_level", "file": "log_file"},
}


def load_config(path: str | Path) -> Config:
    with open(path, "rb") as f:
        data = tomllib.load(f)

    kwargs: dict = {}
    for key, value in data.items():
        if key == "profile":
            kwargs["profile"] = value
        elif key == "alarms":
            kwargs["alarms"] = dict(value)
        elif key in _SCHEMA and isinstance(value, dict):
            for sub, sub_value in value.items():
                if sub not in _SCHEMA[key]:
                    raise ConfigError(f"unknown setting [{key}] {sub}")
                kwargs[_SCHEMA[key][sub]] = sub_value
        else:
            raise ConfigError(f"unknown setting {key}")
    return Config(**kwargs)
