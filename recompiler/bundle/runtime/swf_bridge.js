// swf_bridge.js — the window.__swfBridge contract on a recompiled-game page.
//
// This is the SWFRecomp-CC half of the converged Archipelago "swfrecomp"
// substrate (Mode 1: opaque fixed AVM1 minigame). It exposes the stable JS
// bridge that Archipelago-CC's iframe `bridge.js` consumes — see
// ~/CC/Archipelago-CC/NewDocs/plans/procedural-generation/swfrecomp-substrate-converged.md
// and SWFRecompDocs/plans/archipelago-phase3-slice3-reconciliation.md.
//
// Contract consumed by the AP side (host → game / game → host):
//   window.__swfBridge = {
//     configure(payload),     // { gameId, params, ap_items, ap_locations, regionId }
//     pollItems(items),       // received items to apply in-game
//     sendLocation(flashName) // game reports an objective; AP overrides this
//   }
//
// Mechanism (resolved + de-risked 2026-05-31): injected-AS via the recompiled
// runtime's ExternalInterface, pull for inward. The runtime forwards
// `ExternalInterface.call(name, arg)` → `window[name](arg)` and marshals a
// string return value back to AS (only when this shim is loaded — the C handler
// is gated on `window.__swfBridge` existing). So this file bridges the AP-facing
// `__swfBridge` object to three EI-facing top-level window functions the game's
// ActionScript calls:
//   __swfConfig()            → returns the config payload as JSON (inward pull)
//   __swfPoll()              → returns queued flash_names, comma-separated, then
//                              drains (inward pull; '' when nothing pending)
//   __swfSendLocation(name)  → routes to __swfBridge.sendLocation (outward)
//
// Loaded as a plain classic script BEFORE the WASM module (so the C bridge gate
// sees window.__swfBridge at ensureGlobalInit time). No imports — the AP-side
// bridge.js owns the actual host transport; this only exposes the seam.

(function () {
	"use strict";

	// flash_names received from the host, pending pull by the game's AS.
	var itemQueue = [];
	// The last configure() payload (read once by the game via __swfConfig).
	var configPayload = null;
	// AP item name → flash_name, supplied by configure().ap_items. The substrate
	// owns the name mapping; the game only ever sees flash_names.
	var apItems = {};

	function itemToApName(it) {
		if (it == null) return null;
		if (typeof it === "string") return it;
		// Tolerate a few shapes the host snapshot might use.
		if (it.name != null) return it.name;
		if (it.itemName != null) return it.itemName;
		if (it.item != null) return it.item;
		return null;
	}

	window.__swfBridge = {
		// host → game: select/parameterize the level + supply the item-name map.
		configure: function (payload) {
			configPayload = payload || {};
			apItems = (configPayload && configPayload.ap_items) || {};
			console.log("[swfBridge] configure:", JSON.stringify(configPayload));
		},

		// host → game: enqueue received items (mapped AP name → flash_name) for
		// the game's per-frame pull. Items already absent from ap_items but given
		// as a bare flash_name string pass through unchanged.
		pollItems: function (items) {
			if (!items || !items.length) return;
			for (var i = 0; i < items.length; i++) {
				var apName = itemToApName(items[i]);
				if (apName == null) continue;
				var flash = apItems[apName];
				if (flash === undefined) {
					// Fallback: treat a bare string as a flash_name directly.
					if (typeof items[i] === "string") flash = items[i];
				}
				if (flash !== undefined && flash !== null) itemQueue.push(flash);
			}
		},

		// game → host: an in-game objective completed. Default just logs (so
		// standalone / headed-test runs are visible); Archipelago-CC's bridge.js
		// overrides this to publish user:locationCheck on the dispatcher.
		sendLocation: function (flashName) {
			console.log("[swfBridge] sendLocation (unhandled):", flashName);
		},
	};

	// ── EI-facing helpers the recompiled game's ActionScript calls ────────────
	// (top-level window functions so ExternalInterface.call(name, ...) resolves).

	// Inward pull: drain and return queued flash_names, comma-separated.
	window.__swfPoll = function () {
		if (itemQueue.length === 0) return "";
		var s = itemQueue.join(",");
		itemQueue = [];
		return s;
	};

	// Inward pull: the game reads its configure() payload once, as JSON.
	window.__swfConfig = function () {
		return configPayload ? JSON.stringify(configPayload) : "";
	};

	// Outward: route the game's objective completion to the contract's
	// sendLocation (picks up Archipelago-CC's override if present).
	window.__swfSendLocation = function (flashName) {
		try {
			window.__swfBridge.sendLocation(flashName);
		} catch (e) {
			if (window.console) console.error("[swfBridge] sendLocation failed:", e);
		}
		return "";
	};

	console.log("[swfBridge] installed");
})();
