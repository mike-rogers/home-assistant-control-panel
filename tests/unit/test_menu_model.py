import copy

import pytest

from kitchen_panel.menu_model import (
    MAX_LIST,
    MenuError,
    find_item,
    state_entities,
    to_device,
    validate_menu,
)

SPEC_MENU = {
    "title": "Kitchen",
    "items": [
        {
            "id": "wake",
            "label": "Wake-up",
            "type": "submenu",
            "items": [
                {
                    "id": "wake_time",
                    "label": "Tomorrow's wake-up",
                    "type": "time",
                    "entity": "input_datetime.house_wakeup_time",
                },
                {
                    "id": "wake_snow_day",
                    "label": "Snow day (sleep in)",
                    "type": "action",
                    "target": "script.snow_day",
                    "undo": True,
                },
            ],
        },
        {
            "id": "goodnight",
            "label": "Good night",
            "type": "action",
            "target": "script.good_night",
            "undo": True,
        },
        {
            "id": "automations",
            "label": "Automations",
            "type": "submenu",
            "items": [
                {
                    "id": "summon_kids",
                    "label": "Summon the kids",
                    "type": "action",
                    "target": "script.summon_kids",
                },
                {
                    "id": "outside",
                    "label": "Outside",
                    "type": "info",
                    "entity": "sensor.outdoor_temperature",
                },
            ],
        },
    ],
}


def test_spec_menu_normalizes_defaults():
    menu = validate_menu(SPEC_MENU)
    wake_time = find_item(menu, "wake_time")
    assert wake_time["step_minutes"] == 15
    assert wake_time["undo"] is True
    kids = find_item(menu, "summon_kids")
    assert kids["undo"] is False
    assert kids["confirm"] is None
    assert kids["snapshot"] is None


def test_validation_does_not_mutate_input():
    original = copy.deepcopy(SPEC_MENU)
    validate_menu(SPEC_MENU)
    assert SPEC_MENU == original


def test_device_form_strips_targets_and_snapshots():
    device = to_device(validate_menu(SPEC_MENU))
    assert device["schema"] == 1
    assert device["title"] == "Kitchen"
    goodnight = device["items"][1]
    assert goodnight == {
        "id": "goodnight",
        "label": "Good night",
        "type": "action",
        "undo": True,
    }
    # confirm only appears when set explicitly.
    kids = device["items"][2]["items"][0]
    assert "confirm" not in kids and "target" not in kids
    assert device["items"][0]["items"][0] == {
        "id": "wake_time",
        "label": "Tomorrow's wake-up",
        "type": "time",
        "entity": "input_datetime.house_wakeup_time",
        "step_minutes": 15,
        "undo": True,
    }


def test_state_entities():
    assert state_entities(validate_menu(SPEC_MENU)) == {
        "input_datetime.house_wakeup_time",
        "sensor.outdoor_temperature",
    }


@pytest.mark.parametrize(
    ("mutate", "fragment"),
    [
        (lambda m: m["items"][1].update(id="Good Night"), "ids are"),
        (lambda m: m["items"][1].update(target="light.kitchen"), "must be one of"),
        (lambda m: m["items"][1].pop("target"), "target"),
        (lambda m: m["items"][1].update(label=""), "label"),
        (lambda m: m["items"][1].update(label="x" * 48), "longer than 47"),
        (lambda m: m["items"][1].update(type="slider"), "type must be"),
        (lambda m: m["items"][2]["items"][0].update(id="goodnight"), "duplicate"),
        (
            lambda m: m["items"][0]["items"][0].update(entity="sensor.wake"),
            "must be one of",
        ),
        (lambda m: m["items"][0]["items"][0].update(step_minutes=0), "at least"),
        (lambda m: m["items"][1].update(snapshot=["nope"]), "entity id"),
        (lambda m: m["items"].append(42), "must be an object"),
    ],
)
def test_rejects(mutate, fragment):
    menu = copy.deepcopy(SPEC_MENU)
    mutate(menu)
    with pytest.raises(MenuError, match=fragment):
        validate_menu(menu)


def test_depth_limit():
    leaf = {"id": "c", "label": "C", "type": "info", "entity": "sensor.x"}
    three = {
        "items": [
            {"id": "a", "label": "A", "type": "submenu", "items": [
                {"id": "b", "label": "B", "type": "submenu", "items": [leaf]},
            ]},
        ]
    }
    validate_menu(three)
    three["items"][0]["items"][0]["items"] = [
        {"id": "d", "label": "D", "type": "submenu", "items": []}
    ]
    with pytest.raises(MenuError, match="nest at most 3"):
        validate_menu(three)


def test_list_and_total_limits():
    def action(i):
        return {"id": f"a{i}", "label": "A", "type": "action",
                "target": "script.x"}

    with pytest.raises(MenuError, match=f"more than {MAX_LIST} items"):
        validate_menu({"items": [action(i) for i in range(MAX_LIST + 1)]})

    subs = [
        {"id": f"s{s}", "label": "S", "type": "submenu",
         "items": [action(s * 100 + i) for i in range(MAX_LIST)]}
        for s in range(4)
    ]
    with pytest.raises(MenuError, match="in total"):
        validate_menu({"items": subs})


def test_empty_menu_is_valid():
    assert to_device(validate_menu({})) == {"schema": 1, "title": "", "items": []}
