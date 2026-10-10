/* Kitchen Panel card: compose a panel's menu.
 * Served by the kitchen_panel integration; no build step, no dependencies.
 * Usage in a dashboard:  type: custom:kitchen-panel-card
 *                        entry_id: <optional; defaults to the first panel>
 *
 * Every change is validated and published by the integration immediately,
 * so the panel always shows what this card shows.
 */

const CARD_VERSION = "0.1.0";
console.info(`[kitchen-panel-card] v${CARD_VERSION}`);

const MAX_DEPTH = 3;
const ACTION_DOMAINS = ["script", "automation", "scene", "button"];
const TIME_DOMAINS = ["input_datetime", "time"];
const RESTORABLE_DOMAINS = [
  "climate", "cover", "fan", "humidifier", "input_boolean", "input_datetime",
  "input_number", "input_select", "input_text", "light", "lock",
  "media_player", "number", "select", "siren", "switch", "valve",
  "water_heater",
];
const TYPE_INFO = {
  action: { name: "Action", icon: "mdi:play-circle-outline" },
  time: { name: "Time setting", icon: "mdi:clock-edit-outline" },
  info: { name: "Show a value", icon: "mdi:information-outline" },
  submenu: { name: "Submenu", icon: "mdi:folder-outline" },
};

const esc = (s) =>
  String(s ?? "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[c]);

const clone = (o) => JSON.parse(JSON.stringify(o));

// ---- Menu tree helpers (items addressed by index paths, e.g. [2, 0]) ----

function listAt(menu, parentPath) {
  let items = menu.items;
  for (const i of parentPath) items = items[i].items;
  return items;
}

function itemAt(menu, path) {
  return listAt(menu, path.slice(0, -1))[path[path.length - 1]];
}

function allIds(menu) {
  const ids = new Set();
  const walk = (items) => items.forEach((it) => {
    ids.add(it.id);
    if (it.type === "submenu") walk(it.items);
  });
  walk(menu.items);
  return ids;
}

function slugId(label, taken) {
  let base = label.toLowerCase().replace(/[^a-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "").slice(0, 24) || "item";
  let id = base;
  for (let n = 2; taken.has(id); n++) id = `${base}_${n}`;
  return id;
}

function subtreeHeight(item) {
  if (item.type !== "submenu" || !item.items.length) return 1;
  return 1 + Math.max(...item.items.map(subtreeHeight));
}

// Lists an item could live in: top level plus every submenu, as
// {key, label, path, depth} where depth is the list's level (top = 1).
function locations(menu) {
  const out = [{ key: "", label: "Top level", path: [], depth: 1 }];
  const walk = (items, path, depth, trail) => items.forEach((it, i) => {
    if (it.type !== "submenu") return;
    const p = [...path, i];
    const label = [...trail, it.label].join(" › ");
    out.push({ key: p.join("."), label, path: p, depth: depth + 1 });
    walk(it.items, p, depth + 1, [...trail, it.label]);
  });
  walk(menu.items, [], 1, []);
  return out;
}

class KitchenPanelCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._panels = null;
    this._entryId = null;
    this._menu = null;
    this._form = null; // {path|null, parentKey, draft}
    this._error = "";
    this._derived = null; // {target, entities} for the open form
  }

  setConfig(config) {
    this._config = { ...(config || {}) };
    this._entryId = this._config.entry_id || null;
  }

  static getStubConfig() {
    return {};
  }

  getCardSize() {
    return 6;
  }

  set hass(hass) {
    const first = !this._hass;
    this._hass = hass;
    // Entity lists rarely change; re-rendering on every state update would
    // clobber a half-filled form, so only the first hass triggers a load.
    if (first) this._load();
  }

  // ---- Data -------------------------------------------------------------

  async _load() {
    try {
      this._panels = await this._hass.callWS({ type: "kitchen_panel/panels" });
      if (!this._entryId && this._panels.length) {
        this._entryId = this._panels[0].entry_id;
      }
      if (this._entryId) {
        const res = await this._hass.callWS({
          type: "kitchen_panel/menu/get", entry_id: this._entryId,
        });
        this._menu = res.menu;
      }
      this._error = "";
    } catch (err) {
      this._error = err.message || String(err);
    }
    this._render();
  }

  // Validate + store + publish. Returns true on success.
  async _commit(menu) {
    try {
      const res = await this._hass.callWS({
        type: "kitchen_panel/menu/set", entry_id: this._entryId, menu,
      });
      this._menu = res.menu;
      this._error = "";
      return true;
    } catch (err) {
      this._error = err.message || String(err);
      return false;
    } finally {
      this._render();
    }
  }

  async _derive(target) {
    if (!target) {
      this._derived = null;
      return;
    }
    this._derived = { target, entities: null };
    try {
      const res = await this._hass.callWS({
        type: "kitchen_panel/snapshot/derive", target,
      });
      if (this._derived && this._derived.target === target) {
        this._derived.entities = res.entities;
      }
    } catch (err) {
      this._derived = { target, entities: [], error: err.message };
    }
    this._render();
  }

  _entities(domains, filter) {
    return Object.values(this._hass.states)
      .filter((s) => !domains || domains.includes(s.entity_id.split(".")[0]))
      .filter((s) => !filter || filter(s))
      .map((s) => ({
        id: s.entity_id,
        name: s.attributes.friendly_name || s.entity_id,
        domain: s.entity_id.split(".")[0],
      }))
      .sort((a, b) => a.name.localeCompare(b.name));
  }

  _name(entityId) {
    const s = this._hass.states[entityId];
    return (s && s.attributes.friendly_name) || entityId;
  }

  // ---- Tree operations (each one commits) -------------------------------

  _move(path, delta) {
    const menu = clone(this._menu);
    const list = listAt(menu, path.slice(0, -1));
    const i = path[path.length - 1];
    const j = i + delta;
    if (j < 0 || j >= list.length) return;
    [list[i], list[j]] = [list[j], list[i]];
    this._commit(menu);
  }

  _delete(path) {
    const item = itemAt(this._menu, path);
    const what = item.type === "submenu" && item.items.length
      ? `"${item.label}" and the ${item.items.length} item(s) inside it`
      : `"${item.label}"`;
    if (!confirm(`Remove ${what} from the panel?`)) return;
    const menu = clone(this._menu);
    listAt(menu, path.slice(0, -1)).splice(path[path.length - 1], 1);
    this._commit(menu);
  }

  // ---- Form -------------------------------------------------------------

  _openNew(parentPath) {
    this._form = {
      path: null,
      parentKey: parentPath.join("."),
      draft: { type: "action", label: "", target: "", undo: true,
               confirm: null, snapshot: null },
    };
    this._derived = null;
    this._error = "";
    this._render();
  }

  _openEdit(path) {
    const item = clone(itemAt(this._menu, path));
    this._form = { path, parentKey: path.slice(0, -1).join("."), draft: item };
    this._error = "";
    this._derived = null;
    if (item.type === "action") this._derive(item.target);
    this._render();
  }

  _closeForm() {
    this._form = null;
    this._derived = null;
    this._error = "";
    this._render();
  }

  // Read the form's inputs into the draft (called on every change).
  _readForm() {
    const q = (id) => this.shadowRoot.getElementById(id);
    const d = this._form.draft;
    const type = q("f-type").value;
    if (type !== d.type) {
      // Switching type keeps only what every type shares.
      this._form.draft = { id: d.id, label: d.label, type };
      Object.assign(this._form.draft, {
        action: { target: "", undo: true, confirm: null, snapshot: null },
        time: { entity: "", step_minutes: 15, undo: true },
        info: { entity: "" },
        submenu: { items: [] },
      }[type]);
      this._derived = null;
      return;
    }
    d.label = q("f-label").value;
    this._form.parentKey = q("f-where").value;
    if (type === "action") {
      const target = q("f-target").value;
      if (target !== d.target) {
        d.target = target;
        d.snapshot = null; // a new target re-derives
        this._derive(target);
      }
      d.undo = q("f-undo").checked;
      const confirmMode = q("f-confirm").value;
      d.confirm = confirmMode === "auto" ? null : confirmMode === "always";
      // Only read the checklist if it was on screen (custom mode already on);
      // switching custom on seeds it from the derived list instead.
      const wasCustom = d.snapshot !== null;
      const custom = q("f-custom");
      if (custom) {
        if (custom.checked && !wasCustom) {
          d.snapshot = (this._derived?.entities || []).map((e) => e.entity_id);
        } else if (!custom.checked) {
          d.snapshot = null;
        }
      }
      if (wasCustom && d.snapshot !== null) {
        d.snapshot = [...this.shadowRoot.querySelectorAll(".snap:checked")]
          .map((el) => el.value);
        const add = q("f-snap-add");
        if (add && add.value && !d.snapshot.includes(add.value)) {
          d.snapshot.push(add.value);
        }
      }
    } else if (type === "time") {
      d.entity = q("f-entity").value;
      d.step_minutes = parseInt(q("f-step").value, 10) || 15;
      d.undo = q("f-undo").checked;
    } else if (type === "info") {
      d.entity = q("f-entity").value;
    }
  }

  async _saveForm() {
    this._readForm();
    const d = this._form.draft;
    if (!d.label.trim()) {
      this._error = "Give it a label: that's what the panel shows.";
      return this._render();
    }
    if ((d.type === "action" && !d.target) ||
        ((d.type === "time" || d.type === "info") && !d.entity)) {
      this._error = "Choose what it controls.";
      return this._render();
    }
    d.label = d.label.trim();

    const menu = clone(this._menu);
    const taken = allIds(menu);
    if (this._form.path) {
      // Editing: lift it out of its current list first.
      const old = this._form.path;
      listAt(menu, old.slice(0, -1)).splice(old[old.length - 1], 1);
    } else {
      d.id = slugId(d.label, taken);
    }
    // Where it goes. Paths of later siblings may have shifted after the
    // removal above, so resolve the destination by the parent's id.
    const dest = this._resolveDestination(menu, this._form.parentKey);
    if (dest === null) {
      this._error = "That location no longer exists.";
      return this._render();
    }
    if (this._form.path &&
        this._form.parentKey === this._form.path.slice(0, -1).join(".")) {
      dest.splice(this._form.path[this._form.path.length - 1], 0, d);
    } else {
      dest.push(d);
    }
    if (await this._commit(menu)) this._closeForm();
  }

  _resolveDestination(menu, parentKey) {
    if (parentKey === "") return menu.items;
    // Map the key (index path in the menu as rendered) to the parent's id,
    // then find that id in the modified menu.
    const parent = itemAt(this._menu, parentKey.split(".").map(Number));
    let found = null;
    const walk = (items) => items.forEach((it) => {
      if (it.id === parent.id) found = it.items;
      else if (it.type === "submenu") walk(it.items);
    });
    walk(menu.items);
    return found;
  }

  // ---- Rendering ----------------------------------------------------------

  _renderTree(items, path, depth) {
    if (!items.length) {
      return `<div class="empty">${depth === 1
        ? "No items yet. The panel shows an empty menu."
        : "Empty submenu."}</div>`;
    }
    return `<ul>${items.map((it, i) => {
      const p = [...path, i];
      const key = p.join(".");
      const info = TYPE_INFO[it.type] || { name: it.type, icon: "mdi:help" };
      const detail = it.type === "action" ? this._name(it.target)
        : it.type === "submenu" ? `${it.items.length} item(s)`
        : this._name(it.entity);
      const flags = [];
      if (it.type === "action" || it.type === "time") {
        if (it.undo) flags.push("Oops");
        const asks = it.confirm === null || it.confirm === undefined
          ? !it.undo : it.confirm;
        if (it.type === "action" && asks) flags.push("asks first");
      }
      const children = it.type === "submenu"
        ? this._renderTree(it.items, p, depth + 1) +
          (depth < MAX_DEPTH
            ? `<button class="add sub" data-act="add" data-path="${key}">+ Add to ${esc(it.label)}</button>`
            : "")
        : "";
      return `<li>
        <div class="row">
          <ha-icon icon="${info.icon}"></ha-icon>
          <div class="text">
            <div class="label">${esc(it.label)}</div>
            <div class="detail">${esc(info.name)} · ${esc(detail)}${
              flags.length ? ` · <span class="flags">${esc(flags.join(", "))}</span>` : ""}</div>
          </div>
          <div class="tools">
            <button title="Move up" data-act="up" data-path="${key}" ${i === 0 ? "disabled" : ""}>↑</button>
            <button title="Move down" data-act="down" data-path="${key}" ${i === items.length - 1 ? "disabled" : ""}>↓</button>
            <button title="Edit" data-act="edit" data-path="${key}">Edit</button>
            <button title="Remove" data-act="del" data-path="${key}">✕</button>
          </div>
        </div>
        ${children}
      </li>`;
    }).join("")}</ul>`;
  }

  _options(entities, selected, placeholder) {
    const groups = {};
    entities.forEach((e) => (groups[e.domain] ||= []).push(e));
    const opts = Object.keys(groups).sort().map((dom) =>
      `<optgroup label="${esc(dom)}">${groups[dom].map((e) =>
        `<option value="${esc(e.id)}" ${e.id === selected ? "selected" : ""}>${esc(e.name)}</option>`
      ).join("")}</optgroup>`).join("");
    return `<option value="" ${selected ? "" : "selected"}>${esc(placeholder)}</option>${opts}`;
  }

  _renderSnapshot(d) {
    if (!d.undo || !d.target) return "";
    const derived = this._derived && this._derived.target === d.target
      ? this._derived.entities : null;
    if (d.snapshot === null) {
      let body;
      if (derived === null) body = "Working out what this changes…";
      else if (!derived.length) {
        body = `<span class="warn">Couldn't tell what this changes (for example, it uses templates), so Oops would have nothing to restore. Choose the entities below.</span>`;
      } else {
        body = derived.map((e) => esc(e.name)).join(", ");
      }
      return `<div class="snapbox">
        <div class="hint">Oops restores: ${body}</div>
        <label><input type="checkbox" id="f-custom"> Choose these myself</label>
      </div>`;
    }
    const chosen = new Set(d.snapshot);
    const listed = [...new Set([...(derived || []).map((e) => e.entity_id), ...d.snapshot])];
    return `<div class="snapbox">
      <label><input type="checkbox" id="f-custom" checked> Choose these myself</label>
      <div class="snaps">${listed.map((id) => `<label><input type="checkbox" class="snap" value="${esc(id)}" ${chosen.has(id) ? "checked" : ""}> ${esc(this._name(id))}</label>`).join("")}</div>
      <select id="f-snap-add">${this._options(
        this._entities(RESTORABLE_DOMAINS).filter((e) => !chosen.has(e.id)), "", "Add an entity…")}</select>
    </div>`;
  }

  _renderForm() {
    const f = this._form;
    const d = f.draft;
    const editingPath = f.path ? f.path.join(".") : null;
    // A submenu can't move into itself, and nothing may nest past 3 levels.
    const height = f.path ? subtreeHeight(d) : 1;
    const where = locations(this._menu).filter((loc) =>
      (editingPath === null ||
        (loc.key !== editingPath && !loc.key.startsWith(`${editingPath}.`))) &&
      loc.depth + height - 1 <= MAX_DEPTH);
    // Changing a non-empty submenu's kind would silently drop its items.
    const lockType = d.type === "submenu" && d.items && d.items.length > 0;
    const typeSel = Object.entries(TYPE_INFO).map(([k, v]) =>
      `<option value="${k}" ${k === d.type ? "selected" : ""}>${v.name}</option>`).join("");

    let specific = "";
    if (d.type === "action") {
      const confirmMode = d.confirm === null || d.confirm === undefined ? "auto"
        : d.confirm ? "always" : "never";
      specific = `
        <label>Runs<select id="f-target">${this._options(
          this._entities(ACTION_DOMAINS), d.target, "Choose a script, automation, scene or button…")}</select></label>
        <label class="check"><input type="checkbox" id="f-undo" ${d.undo ? "checked" : ""}> Offer "Oops" (undo) for 30 seconds</label>
        ${this._renderSnapshot(d)}
        <label>Ask "are you sure?"<select id="f-confirm">
          <option value="auto" ${confirmMode === "auto" ? "selected" : ""}>Only when it can't be undone</option>
          <option value="always" ${confirmMode === "always" ? "selected" : ""}>Always</option>
          <option value="never" ${confirmMode === "never" ? "selected" : ""}>Never</option>
        </select></label>`;
    } else if (d.type === "time") {
      specific = `
        <label>Time to change<select id="f-entity">${this._options(
          this._entities(TIME_DOMAINS, (s) =>
            s.entity_id.startsWith("time.") || s.attributes.has_time),
          d.entity, "Choose a time helper…")}</select></label>
        <label>Each click of the wheel changes it by<select id="f-step">${
          [1, 5, 10, 15, 30, 60].map((m) =>
            `<option value="${m}" ${m === d.step_minutes ? "selected" : ""}>${m} min</option>`).join("")}
        </select></label>
        <label class="check"><input type="checkbox" id="f-undo" ${d.undo ? "checked" : ""}> Offer "Oops" (undo) for 30 seconds</label>`;
    } else if (d.type === "info") {
      specific = `<label>Value to show<select id="f-entity">${this._options(
        this._entities(null), d.entity, "Choose an entity…")}</select></label>`;
    }

    return `<div class="form">
      <h3>${f.path ? "Edit item" : "New item"}</h3>
      <label>Kind<select id="f-type" ${lockType ? "disabled" : ""}>${typeSel}</select></label>
      ${lockType ? `<div class="hint">Empty this submenu to change its kind.</div>` : ""}
      <label>Label on the panel<input id="f-label" maxlength="40" value="${esc(d.label)}" placeholder="e.g. Good night"></label>
      <label>Where<select id="f-where">${where.map((loc) =>
        `<option value="${loc.key}" ${loc.key === f.parentKey ? "selected" : ""}>${esc(loc.label)}</option>`).join("")}</select></label>
      ${specific}
      <div class="buttons">
        <button data-act="cancel">Cancel</button>
        <button class="primary" data-act="save">${f.path ? "Save" : "Add"}</button>
      </div>
    </div>`;
  }

  _render() {
    if (!this._hass) return;
    let body;
    if (this._panels === null) {
      body = `<div class="empty">Loading…</div>`;
    } else if (!this._panels.length) {
      body = `<div class="empty">No Kitchen Panel is set up yet. Add one under Settings → Devices &amp; services.</div>`;
    } else if (!this._menu) {
      body = `<div class="empty">This panel isn't loaded.</div>`;
    } else {
      const chooser = this._panels.length > 1
        ? `<select id="panel">${this._panels.map((p) =>
            `<option value="${p.entry_id}" ${p.entry_id === this._entryId ? "selected" : ""}>${esc(p.title)}</option>`).join("")}</select>`
        : "";
      body = `
        ${chooser}
        ${this._form ? this._renderForm() : `
          ${this._renderTree(this._menu.items, [], 1)}
          <button class="add" data-act="add" data-path="">+ Add item</button>`}`;
    }
    const panel = this._panels?.find((p) => p.entry_id === this._entryId);
    this.shadowRoot.innerHTML = `
      <style>
        ha-card { padding: 12px 16px 16px; }
        h2 { margin: 0 0 8px; font-size: 1.2em; font-weight: 500; }
        h3 { margin: 0 0 8px; font-weight: 500; }
        ul { list-style: none; margin: 0; padding: 0; }
        li ul { margin-left: 22px; border-left: 2px solid var(--divider-color); padding-left: 8px; }
        .row { display: flex; align-items: center; gap: 10px; padding: 6px 0; }
        .row ha-icon { color: var(--secondary-text-color); flex: none; }
        .text { flex: 1; min-width: 0; }
        .label { font-weight: 500; }
        .detail { color: var(--secondary-text-color); font-size: 0.85em;
                  overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .flags { color: var(--primary-color); }
        .tools { display: flex; gap: 4px; flex: none; }
        button { font: inherit; cursor: pointer; border-radius: 6px; padding: 4px 8px;
                 border: 1px solid var(--divider-color); background: none;
                 color: var(--primary-text-color); }
        button:disabled { opacity: 0.35; cursor: default; }
        button.primary { background: var(--primary-color); color: var(--text-primary-color);
                         border-color: var(--primary-color); }
        button.add { margin-top: 8px; color: var(--primary-color); border-style: dashed; }
        button.add.sub { margin: 2px 0 6px; font-size: 0.9em; }
        .empty { color: var(--secondary-text-color); padding: 8px 0; }
        .error { color: var(--error-color); margin: 8px 0; }
        .form label { display: flex; flex-direction: column; gap: 4px; margin: 10px 0; }
        .form label.check, .snapbox label { flex-direction: row; align-items: center; gap: 8px; margin: 6px 0; }
        select, input:not([type]) { font: inherit; padding: 6px; border-radius: 6px;
                 border: 1px solid var(--divider-color); background: var(--card-background-color);
                 color: var(--primary-text-color); }
        #panel { margin-bottom: 8px; }
        .snapbox { margin: 4px 0 8px 26px; }
        .hint { color: var(--secondary-text-color); font-size: 0.9em; }
        .warn { color: var(--warning-color); }
        .snaps { display: flex; flex-direction: column; }
        .buttons { display: flex; justify-content: flex-end; gap: 8px; margin-top: 12px; }
      </style>
      <ha-card>
        <h2>${esc(this._config?.title || panel?.title || "Kitchen Panel")} menu</h2>
        ${this._error ? `<div class="error">${esc(this._error)}</div>` : ""}
        ${body}
      </ha-card>`;
    this._wire();
  }

  _wire() {
    const root = this.shadowRoot;
    root.querySelectorAll("button[data-act]").forEach((btn) => {
      btn.addEventListener("click", () => {
        const path = btn.dataset.path === "" || btn.dataset.path === undefined
          ? [] : btn.dataset.path.split(".").map(Number);
        switch (btn.dataset.act) {
          case "up": return this._move(path, -1);
          case "down": return this._move(path, 1);
          case "edit": return this._openEdit(path);
          case "del": return this._delete(path);
          case "add": return this._openNew(path);
          case "cancel": return this._closeForm();
          case "save": return this._saveForm();
        }
      });
    });
    const panel = root.getElementById("panel");
    if (panel) {
      panel.addEventListener("change", () => {
        this._entryId = panel.value;
        this._form = null;
        this._load();
      });
    }
    if (this._form) {
      // Structural inputs re-render the form; plain text doesn't need to.
      root.querySelectorAll(".form select, .form input[type=checkbox]")
        .forEach((el) => el.addEventListener("change", () => {
          this._readForm();
          this._render();
        }));
      const label = root.getElementById("f-label");
      if (label) label.addEventListener("input", () => (this._form.draft.label = label.value));
    }
  }
}

if (!customElements.get("kitchen-panel-card")) {
  customElements.define("kitchen-panel-card", KitchenPanelCard);
}
window.customCards = window.customCards || [];
if (!window.customCards.some((c) => c.type === "kitchen-panel-card")) {
  window.customCards.push({
    type: "kitchen-panel-card",
    name: "Kitchen Panel menu",
    description: "Compose what a Kitchen Panel's menu shows and runs.",
  });
}
