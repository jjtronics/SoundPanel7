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
    CONF_HA_TOKEN,
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
            ha_token = user_input[CONF_HA_TOKEN].strip()

            info, error_key = await self._async_probe(host, port, api_path, ha_token)
            if info is None:
                errors["base"] = error_key or "cannot_connect"
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
                vol.Required(CONF_HA_TOKEN): str,
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

        self.context["title_placeholders"] = {"name": name}
        self._discovered_info = {
            CONF_HOST: host,
            CONF_PORT: port,
            CONF_API_PATH: api_path,
            CONF_NAME: name,
            CONF_MAC: mac.upper(),
            CONF_MODEL: _property(properties, "model", "SoundPanel 7"),
            CONF_MANUFACTURER: _property(properties, "manufacturer", "JJ"),
            CONF_SW_VERSION: _property(properties, "version", ""),
        }
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(self, user_input: dict[str, Any] | None = None):
        errors: dict[str, str] = {}

        if user_input is not None:
            ha_token = user_input[CONF_HA_TOKEN].strip()
            info, error_key = await self._async_probe(
                self._discovered_info[CONF_HOST],
                self._discovered_info[CONF_PORT],
                self._discovered_info[CONF_API_PATH],
                ha_token,
                fallback_name=self._discovered_info[CONF_NAME],
                fallback_mac=self._discovered_info[CONF_MAC],
            )
            if info is None:
                errors["base"] = error_key or "cannot_connect"
            else:
                self._discovered_info = info
                return self.async_create_entry(
                    title=self._discovered_info[CONF_NAME],
                    data=self._discovered_info,
                )

        return self.async_show_form(
            step_id="zeroconf_confirm",
            data_schema=vol.Schema({vol.Required(CONF_HA_TOKEN): str}),
            errors=errors,
            description_placeholders={"name": self._discovered_info[CONF_NAME], "host": self._discovered_info[CONF_HOST]},
        )

    async def _async_probe(
        self,
        host: str,
        port: int,
        api_path: str,
        ha_token: str,
        fallback_name: str | None = None,
        fallback_mac: str | None = None,
        properties: dict[str, Any] | None = None,
    ) -> tuple[dict[str, Any] | None, str | None]:
        session = async_get_clientsession(self.hass)
        url = f"http://{host}:{port}{api_path}"
        headers = {"Authorization": f"Bearer {ha_token}"}

        try:
            response = await session.get(url, headers=headers, timeout=10)
            if response.status == 401:
                return None, "invalid_auth"
            if response.status == 403:
                return None, "token_not_configured"
            response.raise_for_status()
            payload = await response.json()
        except (ClientError, TimeoutError, ValueError):
            return None, "cannot_connect"

        if not isinstance(payload, dict):
            return None, "cannot_connect"

        props = properties or {}
        name = str(payload.get(CONF_NAME) or fallback_name or _property(props, "name", host))
        mac = str(payload.get(CONF_MAC) or fallback_mac or _property(props, "mac", host)).upper()

        return {
            CONF_HOST: host,
            CONF_PORT: port,
            CONF_API_PATH: api_path,
            CONF_HA_TOKEN: ha_token,
            CONF_NAME: name,
            CONF_MAC: mac,
            CONF_MODEL: str(payload.get(CONF_MODEL) or _property(props, "model", "SoundPanel 7")),
            CONF_MANUFACTURER: str(payload.get(CONF_MANUFACTURER) or _property(props, "manufacturer", "JJ")),
            CONF_SW_VERSION: str(payload.get(CONF_SW_VERSION) or _property(props, "version", "")),
        }, None
