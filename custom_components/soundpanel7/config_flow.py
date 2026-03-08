from __future__ import annotations

from typing import Any

import voluptuous as vol

from aiohttp import ClientError

from homeassistant.components.zeroconf import ZeroconfServiceInfo
from homeassistant.config_entries import ConfigFlow
from homeassistant.const import CONF_HOST, CONF_NAME, CONF_PORT
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import (
    CONF_API_PATH,
    CONF_MAC,
    CONF_MANUFACTURER,
    CONF_MODEL,
    CONF_SW_VERSION,
    DEFAULT_API_PATH,
    DEFAULT_PORT,
    DOMAIN,
)


def _property(properties: dict[str, Any], key: str, default: str = "") -> str:
    value = properties.get(key, default)
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="ignore")
    return str(value)


class SoundPanel7ConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SoundPanel 7."""

    VERSION = 1
    _discovered_info: dict[str, Any]

    async def async_step_user(self, user_input: dict[str, Any] | None = None):
        errors: dict[str, str] = {}

        if user_input is not None:
            host = user_input[CONF_HOST].strip()
            port = int(user_input.get(CONF_PORT, DEFAULT_PORT))
            api_path = user_input.get(CONF_API_PATH, DEFAULT_API_PATH).strip() or DEFAULT_API_PATH

            info = await self._async_probe(host, port, api_path)
            if info is None:
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(info[CONF_MAC])
                self._abort_if_unique_id_configured(updates={CONF_HOST: host, CONF_PORT: port, CONF_API_PATH: api_path})
                return self.async_create_entry(
                    title=info[CONF_NAME],
                    data=info,
                )

        schema = vol.Schema(
            {
                vol.Required(CONF_HOST): str,
                vol.Optional(CONF_PORT, default=DEFAULT_PORT): int,
                vol.Optional(CONF_API_PATH, default=DEFAULT_API_PATH): str,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo):
        properties = discovery_info.properties
        host = discovery_info.host.rstrip(".")
        port = discovery_info.port or DEFAULT_PORT
        api_path = _property(properties, "api_path", DEFAULT_API_PATH) or DEFAULT_API_PATH
        name = _property(properties, "name", discovery_info.hostname.rstrip("."))
        mac = _property(properties, "mac", discovery_info.hostname.rstrip("."))

        await self.async_set_unique_id(mac)
        self._abort_if_unique_id_configured(updates={CONF_HOST: host, CONF_PORT: port, CONF_API_PATH: api_path})

        info = await self._async_probe(host, port, api_path, fallback_name=name, fallback_mac=mac, properties=properties)
        if info is None:
            return self.async_abort(reason="cannot_connect")

        self.context["title_placeholders"] = {"name": info[CONF_NAME]}
        self._discovered_info = info
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(self, user_input: dict[str, Any] | None = None):
        if user_input is not None:
            return self.async_create_entry(
                title=self._discovered_info[CONF_NAME],
                data=self._discovered_info,
            )

        return self.async_show_form(
            step_id="zeroconf_confirm",
            description_placeholders={"name": self._discovered_info[CONF_NAME], "host": self._discovered_info[CONF_HOST]},
        )

    async def _async_probe(
        self,
        host: str,
        port: int,
        api_path: str,
        fallback_name: str | None = None,
        fallback_mac: str | None = None,
        properties: dict[str, Any] | None = None,
    ) -> dict[str, Any] | None:
        session = async_get_clientsession(self.hass)
        url = f"http://{host}:{port}{api_path}"

        try:
            response = await session.get(url, timeout=10)
            response.raise_for_status()
            payload = await response.json()
        except (ClientError, TimeoutError, ValueError):
            return None

        if not isinstance(payload, dict):
            return None

        props = properties or {}
        name = fallback_name or _property(props, "name", host)
        mac = (fallback_mac or _property(props, "mac", host)).upper()

        return {
            CONF_HOST: host,
            CONF_PORT: port,
            CONF_API_PATH: api_path,
            CONF_NAME: name,
            CONF_MAC: mac,
            CONF_MODEL: _property(props, "model", "SoundPanel 7"),
            CONF_MANUFACTURER: _property(props, "manufacturer", "JJ"),
            CONF_SW_VERSION: _property(props, "version", ""),
        }
