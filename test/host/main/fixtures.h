#pragma once

// The example menu from spec §5.
static const char *SPEC_MENU =
    "{\"schema\":1,\"title\":\"Kitchen\",\"items\":["
    "{\"id\":\"wake\",\"label\":\"Wake-up\",\"type\":\"submenu\",\"items\":["
    "{\"id\":\"wake_time\",\"label\":\"Tomorrow's wake-up\",\"type\":\"time\","
    "\"entity\":\"input_datetime.house_wakeup_time\",\"step_minutes\":15},"
    "{\"id\":\"wake_snow_day\",\"label\":\"Snow day (sleep in)\","
    "\"type\":\"action\",\"undo\":true}]},"
    "{\"id\":\"goodnight\",\"label\":\"Good night\",\"type\":\"action\","
    "\"undo\":true},"
    "{\"id\":\"automations\",\"label\":\"Automations\",\"type\":\"submenu\","
    "\"items\":["
    "{\"id\":\"brighten_living\",\"label\":\"Brighten living room\","
    "\"type\":\"action\",\"undo\":true},"
    "{\"id\":\"summon_kids\",\"label\":\"Summon the kids\",\"type\":\"action\","
    "\"undo\":false,\"confirm\":true}]}]}";

#define WAKE_ENTITY "input_datetime.house_wakeup_time"
