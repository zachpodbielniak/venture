/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The deals board, dragged. Progressive enhancement over the server-drawn
 * board: every card keeps its stage selector and Move button, which is
 * also the keyboard path, and this file only adds dragging a card onto
 * another stage's column. A drop posts the same form the Move button does
 * -- the stage and the version the card was drawn at -- to the same route,
 * so the move goes through the one deal service, and a deal somebody else
 * changed meanwhile is refused there and stays where it was.
 */
(function () {
	"use strict";

	var board = document.querySelector("[data-deal-board]");
	var dragged = null;
	var status;

	if (!board || !window.fetch || !window.URLSearchParams)
		return;

	/* One live region above the board, made here so a page without the
	 * script carries none. */
	status = document.createElement("div");
	status.setAttribute("role", "status");
	status.setAttribute("aria-live", "polite");
	status.className = "board-status";
	board.parentNode.insertBefore(status, board);

	function say(text, refused) {
		status.textContent = text;
		status.setAttribute("role", refused ? "alert" : "status");
		status.setAttribute("data-refused", refused ? "true" : "false");
	}

	function closest(node, attribute) {
		while (node && node !== board) {
			if (node.nodeType === 1 && node.hasAttribute(attribute))
				return node;
			node = node.parentNode;
		}
		return null;
	}

	function unmark() {
		var marked = board.querySelectorAll("[data-drop-target]");
		var i;
		for (i = 0; i < marked.length; i++)
			marked[i].removeAttribute("data-drop-target");
	}

	function accepts(column) {
		return column && dragged &&
			column.getAttribute("data-pipeline-id") === dragged.getAttribute("data-pipeline-id") &&
			column !== closest(dragged, "data-stage-id");
	}

	/* After a move the card's own form must describe the deal as it is
	 * now, or the next Move would be refused as stale. */
	function redraw(card, column, version) {
		var select = card.querySelector("select[name=stage_id]");
		var hidden = card.querySelector("input[name=version]");
		var stage = column.getAttribute("data-stage-id");
		var cards = column.querySelector("[data-stage-cards]") || column;
		card.setAttribute("data-deal-version", String(version));
		if (hidden)
			hidden.value = String(version);
		if (select)
			select.value = stage;
		cards.appendChild(card);
	}

	function message(data, fallback) {
		if (data && typeof data.message === "string" && data.message.length)
			return data.message;
		return fallback;
	}

	function move(card, column) {
		var body = new URLSearchParams();
		var id = card.getAttribute("data-deal-id");
		var title = column.getAttribute("aria-label") || "the new stage";

		body.set("stage_id", column.getAttribute("data-stage-id"));
		body.set("version", card.getAttribute("data-deal-version"));
		card.setAttribute("aria-busy", "true");
		fetch("/deals/" + encodeURIComponent(id) + "/move", {
			method: "POST",
			credentials: "same-origin",
			headers: {
				"Accept": "application/json",
				"Content-Type": "application/x-www-form-urlencoded"
			},
			body: body.toString()
		}).then(function (response) {
			return response.json().then(function (data) {
				return { ok: response.ok, data: data };
			}, function () {
				return { ok: response.ok, data: null };
			});
		}).then(function (result) {
			card.removeAttribute("aria-busy");
			if (!result.ok) {
				say(message(result.data, "The deal was not moved."), true);
				return;
			}
			redraw(card, column, result.data && result.data.version ? result.data.version : 0);
			say("Moved to " + title + ".", false);
		}, function () {
			card.removeAttribute("aria-busy");
			say("The move did not reach the server. Nothing changed; try again.", true);
		});
	}

	board.addEventListener("dragstart", function (event) {
		var card = closest(event.target, "data-deal-version");
		if (!card)
			return;
		dragged = card;
		card.setAttribute("data-dragging", "true");
		event.dataTransfer.effectAllowed = "move";
		event.dataTransfer.setData("text/plain", card.getAttribute("data-deal-id"));
	});

	board.addEventListener("dragend", function () {
		if (dragged)
			dragged.removeAttribute("data-dragging");
		dragged = null;
		unmark();
	});

	board.addEventListener("dragover", function (event) {
		var column = closest(event.target, "data-stage-id");
		if (!accepts(column))
			return;
		event.preventDefault();
		event.dataTransfer.dropEffect = "move";
		if (!column.hasAttribute("data-drop-target")) {
			unmark();
			column.setAttribute("data-drop-target", "true");
		}
	});

	board.addEventListener("drop", function (event) {
		var column = closest(event.target, "data-stage-id");
		var card = dragged;
		unmark();
		if (!accepts(column))
			return;
		event.preventDefault();
		move(card, column);
	});
}());
