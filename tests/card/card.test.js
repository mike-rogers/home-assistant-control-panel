// Drives the real kitchen-panel-card in jsdom against a fake hass whose
// websocket backend stores the menu like the integration does.
//   cd tests/card && npm install && npm test

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { JSDOM } from "jsdom";

const CARD = readFileSync(
  new URL("../../custom_components/kitchen_panel/frontend/kitchen-panel-card.js",
          import.meta.url), "utf8");

const STATES = {
  "script.good_night": { friendly_name: "Good night" },
  "script.summon_kids": { friendly_name: "Summon the kids" },
  "light.kitchen": { friendly_name: "Kitchen light" },
  "light.porch": { friendly_name: "Porch light" },
  "input_datetime.house_wakeup_time": { friendly_name: "Wake-up", has_time: true },
  "input_datetime.birthday": { friendly_name: "Birthday", has_date: true, has_time: false },
  "sensor.outdoor": { friendly_name: "Outside" },
};

function setup(initialMenu = { title: "Kitchen", items: [] }) {
  const dom = new JSDOM("<!doctype html><body></body>", { runScripts: "outside-only" });
  const { window } = dom;
  window.console.info = () => {};
  window.confirm = () => true;
  window.eval(CARD);

  const backend = { menu: structuredClone(initialMenu), sets: [], failNext: null };
  const hass = {
    states: Object.fromEntries(Object.entries(STATES).map(([id, attributes]) =>
      [id, { entity_id: id, state: "on", attributes }])),
    async callWS(msg) {
      switch (msg.type) {
        case "kitchen_panel/panels":
          return [{ entry_id: "e1", title: "Kitchen Panel", device_id: "panel-1" }];
        case "kitchen_panel/menu/get":
          return { menu: structuredClone(backend.menu) };
        case "kitchen_panel/menu/set":
          if (backend.failNext) {
            const message = backend.failNext;
            backend.failNext = null;
            throw { message };
          }
          backend.sets.push(structuredClone(msg.menu));
          backend.menu = structuredClone(msg.menu);
          return { menu: structuredClone(backend.menu) };
        case "kitchen_panel/snapshot/derive":
          return { entities: msg.target === "script.good_night"
            ? [{ entity_id: "light.kitchen", name: "Kitchen light" }] : [] };
      }
      throw new Error(`unexpected ${msg.type}`);
    },
  };

  const card = window.document.createElement("kitchen-panel-card");
  window.document.body.appendChild(card);
  card.setConfig({});
  card.hass = hass;
  const root = card.shadowRoot;
  const settle = () => new Promise((r) => setTimeout(r, 0));
  const $ = (sel) => root.querySelector(sel);
  const click = async (act, path) => {
    const sel = path === undefined ? `button[data-act="${act}"]`
      : `button[data-act="${act}"][data-path="${path}"]`;
    const btn = $(sel);
    assert.ok(btn, `no button ${sel}`);
    btn.click();
    await settle();
  };
  const set = async (id, value) => {
    const el = root.getElementById(id);
    assert.ok(el, `no #${id}`);
    if (el.type === "checkbox") el.checked = value; else el.value = value;
    el.dispatchEvent(new window.Event(id === "f-label" ? "input" : "change"));
    await settle();
  };
  const labels = () => [...root.querySelectorAll(".label")].map((e) => e.textContent);
  return { backend, settle, click, set, $, root, labels };
}

const GOODNIGHT = { id: "goodnight", label: "Good night", type: "action",
  target: "script.good_night", undo: true, confirm: null, snapshot: null };

test("adds an action with a generated id and publishes it", async () => {
  const t = setup();
  await t.settle();
  await t.click("add", "");
  await t.set("f-label", "Good night!");
  await t.set("f-target", "script.good_night");
  await t.settle(); // derive
  assert.match(t.root.textContent, /Oops restores: Kitchen light/);
  await t.click("save");
  assert.equal(t.backend.sets.length, 1);
  assert.deepEqual(t.backend.menu.items[0], {
    label: "Good night!", type: "action", target: "script.good_night",
    undo: true, confirm: null, snapshot: null, id: "good_night",
  });
  assert.deepEqual(t.labels(), ["Good night!"]);
});

test("refuses to save without a label or target", async () => {
  const t = setup();
  await t.settle();
  await t.click("add", "");
  await t.click("save");
  assert.match(t.root.textContent, /Give it a label/);
  await t.set("f-label", "X");
  await t.click("save");
  assert.match(t.root.textContent, /Choose what it controls/);
  assert.equal(t.backend.sets.length, 0);
});

test("ids stay unique and stable across edits", async () => {
  const t = setup({ title: "K", items: [GOODNIGHT] });
  await t.settle();
  await t.click("add", "");
  await t.set("f-label", "Goodnight");
  await t.set("f-target", "script.summon_kids");
  await t.click("save");
  assert.equal(t.backend.menu.items[1].id, "goodnight_2");

  await t.click("edit", "0");
  await t.set("f-label", "Bedtime");
  await t.click("save");
  assert.equal(t.backend.menu.items[0].id, "goodnight"); // unchanged
  assert.equal(t.backend.menu.items[0].label, "Bedtime");
  assert.deepEqual(t.labels(), ["Bedtime", "Goodnight"]); // same position
});

test("reorders and deletes", async () => {
  const t = setup({ title: "K", items: [
    GOODNIGHT,
    { ...GOODNIGHT, id: "b", label: "B" },
    { ...GOODNIGHT, id: "c", label: "C" },
  ] });
  await t.settle();
  assert.ok(t.$('button[data-act="up"][data-path="0"]').disabled);
  await t.click("down", "0");
  assert.deepEqual(t.backend.menu.items.map((i) => i.id), ["b", "goodnight", "c"]);
  await t.click("del", "2");
  assert.deepEqual(t.labels(), ["B", "Good night"]);
});

test("moves an item into a submenu and keeps the submenu's children", async () => {
  const t = setup({ title: "K", items: [
    GOODNIGHT,
    { id: "wake", label: "Wake-up", type: "submenu", items: [
      { id: "wake_time", label: "Tomorrow", type: "time",
        entity: "input_datetime.house_wakeup_time", step_minutes: 15, undo: true },
    ] },
  ] });
  await t.settle();
  await t.click("edit", "0");
  // Time entities exclude date-only helpers.
  await t.set("f-where", "1");
  await t.click("save");
  assert.deepEqual(t.backend.menu.items.map((i) => i.id), ["wake"]);
  assert.deepEqual(t.backend.menu.items[0].items.map((i) => i.id),
                   ["wake_time", "goodnight"]);

  // A submenu with children can't change kind (it would drop them).
  await t.click("edit", "0");
  assert.ok(t.root.getElementById("f-type").disabled);
  // ...and can't be moved into itself.
  const where = [...t.root.getElementById("f-where").options].map((o) => o.value);
  assert.deepEqual(where, [""]);
});

test("time items list only helpers that hold a time", async () => {
  const t = setup();
  await t.settle();
  await t.click("add", "");
  await t.set("f-type", "time");
  const options = [...t.root.getElementById("f-entity").options].map((o) => o.value);
  assert.deepEqual(options, ["", "input_datetime.house_wakeup_time"]);
});

test("custom snapshot list starts from the derived entities", async () => {
  const t = setup({ title: "K", items: [GOODNIGHT] });
  await t.settle();
  await t.click("edit", "0");
  await t.settle(); // derive
  await t.set("f-custom", true);
  let boxes = [...t.root.querySelectorAll(".snap")];
  assert.deepEqual(boxes.map((b) => [b.value, b.checked]), [["light.kitchen", true]]);

  await t.set("f-snap-add", "light.porch");
  boxes = [...t.root.querySelectorAll(".snap")];
  assert.deepEqual(boxes.map((b) => [b.value, b.checked]),
                   [["light.kitchen", true], ["light.porch", true]]);
  await t.click("save");
  assert.deepEqual(t.backend.menu.items[0].snapshot, ["light.kitchen", "light.porch"]);

  await t.click("edit", "0");
  await t.set("f-custom", false);
  await t.click("save");
  assert.equal(t.backend.menu.items[0].snapshot, null);
});

test("server validation errors are shown and the form stays open", async () => {
  const t = setup();
  await t.settle();
  await t.click("add", "");
  await t.set("f-label", "Good night");
  await t.set("f-target", "script.good_night");
  t.backend.failNext = "items[0]: label longer than 47 bytes";
  await t.click("save");
  assert.match(t.root.textContent, /label longer than 47 bytes/);
  assert.ok(t.root.getElementById("f-label"));
  assert.equal(t.root.getElementById("f-label").value, "Good night");
});
