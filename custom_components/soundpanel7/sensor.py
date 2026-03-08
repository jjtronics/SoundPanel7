from __future__ import annotations

from dataclasses import dataclass

from homeassistant.components.sensor import SensorEntity, SensorEntityDescription, SensorStateClass
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_NAME, EntityCategory, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import CONF_MAC, CONF_MANUFACTURER, CONF_MODEL, CONF_SW_VERSION, DOMAIN
from .coordinator import SoundPanel7Coordinator


@dataclass(frozen=True, kw_only=True)
class SoundPanel7SensorDescription(SensorEntityDescription):
    value_key: str


SENSORS: tuple[SoundPanel7SensorDescription, ...] = (
    SoundPanel7SensorDescription(
        key="db",
        translation_key="db",
        name="dB Instant",
        native_unit_of_measurement="dB",
        state_class=SensorStateClass.MEASUREMENT,
        value_key="db",
    ),
    SoundPanel7SensorDescription(
        key="leq",
        translation_key="leq",
        name="Leq",
        native_unit_of_measurement="dB",
        state_class=SensorStateClass.MEASUREMENT,
        value_key="leq",
    ),
    SoundPanel7SensorDescription(
        key="peak",
        translation_key="peak",
        name="Peak",
        native_unit_of_measurement="dB",
        state_class=SensorStateClass.MEASUREMENT,
        value_key="peak",
    ),
    SoundPanel7SensorDescription(
        key="rssi",
        translation_key="wifi_rssi",
        name="WiFi RSSI",
        native_unit_of_measurement="dBm",
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_key="rssi",
    ),
    SoundPanel7SensorDescription(
        key="ip",
        translation_key="wifi_ip",
        name="WiFi IP",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_key="ip",
    ),
    SoundPanel7SensorDescription(
        key="uptime",
        translation_key="uptime",
        name="Uptime",
        native_unit_of_measurement=UnitOfTime.SECONDS,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_key="uptime_s",
    ),
)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: SoundPanel7Coordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(SoundPanel7Sensor(coordinator, entry, description) for description in SENSORS)


class SoundPanel7Sensor(CoordinatorEntity[SoundPanel7Coordinator], SensorEntity):
    """Representation of a SoundPanel 7 sensor."""

    entity_description: SoundPanel7SensorDescription

    def __init__(self, coordinator: SoundPanel7Coordinator, entry: ConfigEntry, description: SoundPanel7SensorDescription) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self.entity_description = description
        self._attr_has_entity_name = True
        self._attr_unique_id = f"{entry.data[CONF_MAC]}_{description.key}".lower().replace(":", "")

    @property
    def device_info(self) -> DeviceInfo:
        entry = self._entry
        return DeviceInfo(
            identifiers={(DOMAIN, entry.data[CONF_MAC])},
            name=entry.title or entry.data[CONF_NAME],
            manufacturer=entry.data.get(CONF_MANUFACTURER, "JJ"),
            model=entry.data.get(CONF_MODEL, "SoundPanel 7"),
            sw_version=entry.data.get(CONF_SW_VERSION),
            configuration_url=f"http://{entry.data[CONF_HOST]}:{self.coordinator.port}",
        )

    @property
    def native_value(self):
        return self.coordinator.data.get(self.entity_description.value_key)
