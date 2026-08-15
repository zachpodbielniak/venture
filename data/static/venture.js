/*
 * venture.js - VENTURE web UI behaviour
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is presentation: theme, the AI panel, toasts, keyboard
 * shortcuts, small table conveniences. All data movement goes through hx-*
 * attributes rendered by the server, so this file never builds a URL or
 * knows a route -- the one exception is resuming the AI thread named in
 * localStorage, whose URL the server rendered in the first place.
 */

(function (window, document) {
	"use strict";

	var STORAGE_THEME = "venture.theme";
	var STORAGE_PANEL = "venture.ai.open";
	var STORAGE_THREAD = "venture.ai.thread";
	var STORAGE_WIDTH = "venture.ai.width";

	/* ------------------------------------------------------------------ */
	/* Theme                                                               */
	/* ------------------------------------------------------------------ */

	/*
	 * Applying the stored theme before first paint is what prevents the
	 * white flash on a dark-theme reload. This function is also emitted
	 * inline in <head> by the server for exactly that reason; running it
	 * again here is harmless and keeps the logic in one place.
	 */
	function applyTheme(theme) {
		var root = document.documentElement;

		if (theme === "light" || theme === "dark") {
			root.setAttribute("data-theme", theme);
		} else {
			/* "system" means: remove the override and let the
			 * prefers-color-scheme media query decide. */
			root.removeAttribute("data-theme");
		}

		document.querySelectorAll("[data-theme-option]").forEach(function (el) {
			el.classList.toggle("active",
				el.getAttribute("data-theme-option") === theme);
		});
	}

	function storedTheme() {
		try {
			return window.localStorage.getItem(STORAGE_THEME) || "system";
		} catch (e) {
			return "system";
		}
	}

	function setTheme(theme) {
		try {
			window.localStorage.setItem(STORAGE_THEME, theme);
		} catch (e) {
			/* Private browsing with storage disabled: the theme still
			 * applies for this page, it just will not persist. */
		}

		applyTheme(theme);
	}

	function cycleTheme() {
		var order = ["system", "light", "dark"];
		var next = order[(order.indexOf(storedTheme()) + 1) % order.length];

		setTheme(next);
		toast("Theme: " + next, "info");
	}

	/* ------------------------------------------------------------------ */
	/* Toasts                                                              */
	/* ------------------------------------------------------------------ */

	function toastContainer() {
		var container = document.querySelector(".toasts");

		if (!container) {
			container = document.createElement("div");
			container.className = "toasts";
			document.body.appendChild(container);
		}

		return container;
	}

	function toast(message, kind, timeout) {
		var el = document.createElement("div");

		el.className = "toast " + (kind || "");
		el.textContent = message;
		toastContainer().appendChild(el);

		window.setTimeout(function () {
			el.style.opacity = "0";
			window.setTimeout(function () {
				if (el.parentNode) {
					el.parentNode.removeChild(el);
				}
			}, 200);
		}, timeout || 4000);

		return el;
	}

	/* ------------------------------------------------------------------ */
	/* The AI panel                                                        */
	/* ------------------------------------------------------------------ */

	function panel() {
		return document.querySelector(".ai-panel");
	}

	function setPanelOpen(open) {
		var el = panel();

		if (!el) {
			return;
		}

		el.classList.toggle("open", open);
		document.body.classList.toggle("ai-open", open);

		try {
			window.localStorage.setItem(STORAGE_PANEL, open ? "1" : "0");
		} catch (e) {
			/* Not persisting the panel state is not worth failing over. */
		}

		if (open) {
			var input = el.querySelector(".chat-input textarea");

			if (input) {
				/* While the slide-in transition runs the panel is
				 * still visibility:hidden, and focusing a hidden
				 * element silently does nothing. Try now for the
				 * reduced-motion case, and again when the
				 * transition lands for everybody else. */
				input.focus();
				window.setTimeout(function () {
					if (el.classList.contains("open")) {
						input.focus();
					}
				}, 220);
			}

			scrollChatToBottom();
		}
	}

	function togglePanel() {
		var el = panel();

		setPanelOpen(el ? !el.classList.contains("open") : true);
	}

	function scrollChatToBottom() {
		var log = document.querySelector(".chat-log");

		if (log) {
			log.scrollTop = log.scrollHeight;
		}
	}

	function threadInput() {
		return document.getElementById("chat-thread");
	}

	function storedThread() {
		try {
			return window.localStorage.getItem(STORAGE_THREAD) || "";
		} catch (e) {
			return "";
		}
	}

	function rememberThread(id) {
		try {
			if (id) {
				window.localStorage.setItem(STORAGE_THREAD, id);
			} else {
				window.localStorage.removeItem(STORAGE_THREAD);
			}
		} catch (e) {
			/* The conversation still works; it just will not resume. */
		}
	}

	/*
	 * The server names the active thread by out-of-band-swapping the hidden
	 * #chat-thread input. Mirroring that value into localStorage after every
	 * swap is what lets the next page load -- or next week's session -- pick
	 * the conversation back up.
	 */
	function syncThreadFromInput() {
		var input = threadInput();

		if (input) {
			rememberThread(input.value);
		}
	}

	/*
	 * On page load, replay whichever conversation was active. The transcript
	 * request is an ordinary hx GET, so the response's OOB input also
	 * restores the hidden field and the panel title.
	 */
	function resumeThread() {
		var id = storedThread();
		var input = threadInput();

		if (!id || !input || !window.htmx) {
			return;
		}

		input.value = id;
		window.htmx.ajax("GET", "/ui/chat/thread/" + encodeURIComponent(id),
			"#chat-log");
	}

	function startNewThread() {
		var input = threadInput();
		var log = document.getElementById("chat-log");
		var title = document.getElementById("ai-panel-title");

		if (input) {
			input.value = "";
		}

		rememberThread("");

		if (log) {
			log.innerHTML = "";
		}

		if (title) {
			title.textContent = "Ask VENTURE";
		}

		var composer = document.querySelector(".chat-input textarea");

		if (composer) {
			composer.focus();
		}
	}

	/*
	 * The composer submits on Enter and inserts a newline on Shift+Enter,
	 * and grows with its content up to the CSS max-height. The form itself
	 * is an ordinary hx-post, so the send path is entirely server-driven.
	 */
	function wireComposer() {
		var textarea = document.querySelector(".chat-input textarea");

		if (!textarea || textarea.ventureWired) {
			return;
		}

		textarea.ventureWired = true;

		textarea.addEventListener("input", function () {
			textarea.style.height = "auto";
			textarea.style.height = textarea.scrollHeight + "px";
		});

		/*
		 * On send: echo the question into the log immediately with a
		 * typing indicator after it, and clear the box. Waiting for the
		 * server to echo it back made every send feel dropped until the
		 * model finished -- the reply renders both sides, but the
		 * operator's half must not wait on the model's half.
		 *
		 * The hx runtime serialises the form synchronously inside the
		 * submit event, so the zero timeout runs after serialisation
		 * but before the reply.
		 */
		{
			var form = textarea.closest("form");

			if (form && !form.ventureClearWired) {
				form.ventureClearWired = true;
				form.addEventListener("submit", function () {
					var text = textarea.value.trim();
					var log = document.getElementById("chat-log");

					if (text !== "" && log) {
						appendUserEcho(log, text);
						appendTyping(log);
						scrollChatToBottom();
					}

					window.setTimeout(function () {
						textarea.value = "";
						textarea.style.height = "auto";
					}, 0);
				});
			}
		}

		textarea.addEventListener("keydown", function (event) {
			if (event.key === "Enter" && !event.shiftKey) {
				event.preventDefault();

				var form = textarea.closest("form");

				if (form && textarea.value.trim() !== "") {
					form.dispatchEvent(new Event("submit",
						{ bubbles: true, cancelable: true }));
				}
			}
		});
	}

	/*
	 * Built with textContent, never innerHTML: this is the one place user
	 * text enters the DOM without going through the server's escaping. The
	 * markup mirrors the server's transcript render exactly, so the echo
	 * and the stored replay are indistinguishable.
	 */
	function appendUserEcho(log, text) {
		var row = document.createElement("div");
		var avatar = document.createElement("span");
		var content = document.createElement("div");
		var p = document.createElement("p");

		row.className = "msg user";
		avatar.className = "msg-avatar";
		avatar.textContent = "You";
		content.className = "msg-content";
		p.textContent = text;

		content.appendChild(p);
		row.appendChild(avatar);
		row.appendChild(content);
		log.appendChild(row);
	}

	function appendTyping(log) {
		var row = document.createElement("div");

		row.className = "msg ai typing-row";
		row.innerHTML = "<span class=\"msg-avatar\">✦</span>"
			+ "<div class=\"msg-content\"><span class=\"typing\">"
			+ "<span></span><span></span><span></span></span></div>";
		log.appendChild(row);
	}

	/* The reply -- or the failure -- has arrived; the dots have done
	 * their job. */
	function clearTyping() {
		document.querySelectorAll(".typing-row").forEach(function (el) {
			el.remove();
		});
	}

	/*
	 * The panel's width is measured from the right screen edge, so it is
	 * simply the window width minus the pointer's X. Clamped so it can
	 * neither collapse below a usable conversation nor swallow the page.
	 */
	function clampPanelWidth(width) {
		var max = Math.round(window.innerWidth * 0.85);

		return Math.max(320, Math.min(width, max));
	}

	function applyPanelWidth(width) {
		var el = panel();

		if (el) {
			el.style.width = clampPanelWidth(width) + "px";
		}
	}

	function wirePanelResize() {
		var handle = document.querySelector("[data-ai-resize]");
		var el = panel();

		if (!handle || !el || handle.ventureWired) {
			return;
		}

		handle.ventureWired = true;

		/* A stored width from an earlier session, reclamped because the
		 * window it was chosen in may have been wider than this one. */
		try {
			var stored = parseInt(
				window.localStorage.getItem(STORAGE_WIDTH), 10);

			if (stored > 0) {
				applyPanelWidth(stored);
			}
		} catch (e) {
			/* Default width it is. */
		}

		handle.addEventListener("pointerdown", function (event) {
			event.preventDefault();

			/* Capture keeps the drag alive when the pointer outruns
			 * the six-pixel handle, but a drag must survive capture
			 * being refused -- it just gets twitchier. */
			try {
				handle.setPointerCapture(event.pointerId);
			} catch (e) {
				/* Carry on uncaptured. */
			}

			el.classList.add("resizing");
			document.body.classList.add("ai-resizing");
		});

		handle.addEventListener("pointermove", function (event) {
			if (!el.classList.contains("resizing")) {
				return;
			}

			applyPanelWidth(window.innerWidth - event.clientX);
		});

		handle.addEventListener("pointerup", function (event) {
			if (!el.classList.contains("resizing")) {
				return;
			}

			try {
				handle.releasePointerCapture(event.pointerId);
			} catch (e) {
				/* Was never captured. */
			}

			el.classList.remove("resizing");
			document.body.classList.remove("ai-resizing");

			try {
				window.localStorage.setItem(STORAGE_WIDTH,
					String(parseInt(el.style.width, 10)));
			} catch (e) {
				/* The width holds for this page; it just will not
				 * persist. */
			}
		});

		/* Double-click puts the default back and forgets the override. */
		handle.addEventListener("dblclick", function () {
			el.style.width = "";

			try {
				window.localStorage.removeItem(STORAGE_WIDTH);
			} catch (e) {
				/* Nothing stored to forget. */
			}
		});
	}

	/* ------------------------------------------------------------------ */
	/* Keyboard shortcuts                                                  */
	/* ------------------------------------------------------------------ */

	function isTyping(target) {
		var tag = target.tagName;

		return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT"
			|| target.isContentEditable;
	}

	function wireShortcuts() {
		document.addEventListener("keydown", function (event) {
			/* Escape always works, including from inside the composer, so
			 * there is a reliable way out of the panel. */
			if (event.key === "Escape") {
				var open = document.querySelector(".modal-backdrop");

				if (open) {
					open.remove();
					return;
				}

				if (panel() && panel().classList.contains("open")) {
					setPanelOpen(false);
					return;
				}
			}

			/* Ctrl+/ toggles the panel from anywhere, typing or not. */
			if (event.key === "/" && (event.ctrlKey || event.metaKey)) {
				event.preventDefault();
				togglePanel();
				return;
			}

			/* Ctrl+K goes to search from anywhere, like every other tool
			 * with a command bar. */
			if (event.key === "k" && (event.ctrlKey || event.metaKey)) {
				var global = document.querySelector("[data-global-search]");

				if (global) {
					event.preventDefault();
					global.focus();
					global.select();
				}

				return;
			}

			if (isTyping(event.target)) {
				return;
			}

			if (event.key === "/") {
				var search = document.querySelector("[data-search-input]")
					|| document.querySelector("[data-global-search]");

				if (search) {
					event.preventDefault();
					search.focus();
					search.select();
				}

				return;
			}

			if (event.key === "?") {
				event.preventDefault();
				showShortcuts();
				return;
			}

			if (event.key === "t") {
				cycleTheme();
			}
		});
	}

	function showShortcuts() {
		var rows = [
			["/", "Focus search"],
			["Ctrl + K", "Global search"],
			["Ctrl + /", "Toggle the AI panel"],
			["t", "Cycle theme"],
			["Esc", "Close dialog or panel"],
			["?", "This list"]
		];

		var body = rows.map(function (row) {
			return "<tr><td><code>" + row[0] + "</code></td><td>"
				+ row[1] + "</td></tr>";
		}).join("");

		openModal("Keyboard shortcuts",
			"<table class=\"data\"><tbody>" + body + "</tbody></table>");
	}

	/* ------------------------------------------------------------------ */
	/* Modals                                                              */
	/* ------------------------------------------------------------------ */

	function openModal(title, bodyHtml, footHtml) {
		var backdrop = document.createElement("div");

		backdrop.className = "modal-backdrop";
		backdrop.innerHTML =
			"<div class=\"modal\" role=\"dialog\" aria-modal=\"true\">" +
			"<div class=\"modal-head\"><h2>" + title + "</h2>" +
			"<button class=\"btn btn-ghost btn-icon\" data-modal-close " +
			"aria-label=\"Close\">&times;</button></div>" +
			"<div class=\"modal-body\">" + bodyHtml + "</div>" +
			(footHtml ? "<div class=\"modal-foot\">" + footHtml + "</div>" : "") +
			"</div>";

		backdrop.addEventListener("click", function (event) {
			if (event.target === backdrop
			    || event.target.hasAttribute("data-modal-close")) {
				backdrop.remove();
			}
		});

		document.body.appendChild(backdrop);

		if (window.htmx) {
			window.htmx.process(backdrop);
		}

		return backdrop;
	}

	/* ------------------------------------------------------------------ */
	/* Tables                                                              */
	/* ------------------------------------------------------------------ */

	/*
	 * Clicking a row navigates to it, unless the click landed on a control
	 * inside the row -- otherwise a delete button would also open the
	 * record it just removed.
	 */
	function wireRowLinks(root) {
		(root || document).querySelectorAll("tr[data-href]").forEach(function (row) {
			if (row.ventureRowWired) {
				return;
			}

			row.ventureRowWired = true;
			row.style.cursor = "pointer";

			row.addEventListener("click", function (event) {
				if (event.target.closest("a, button, input, select, label")) {
					return;
				}

				window.location.href = row.getAttribute("data-href");
			});
		});
	}

	/* ------------------------------------------------------------------ */
	/* Server-driven notifications                                         */
	/* ------------------------------------------------------------------ */

	/*
	 * The server raises these with the HX-Trigger response header, so a
	 * handler that finished an automation or staged an AI write can surface
	 * that in the UI without the page having to poll for it.
	 */
	function wireServerEvents() {
		document.body.addEventListener("venture:notify", function (event) {
			toast(event.detail.message || "Done",
			      event.detail.kind || "positive");
		});

		document.body.addEventListener("venture:confirmation", function (event) {
			toast((event.detail.summary || "A change is waiting for approval")
			      + " — review it in the AI panel", "warning", 8000);
			setPanelOpen(true);
		});

		document.body.addEventListener("venture:refresh", function () {
			window.location.reload();
		});

		document.body.addEventListener("htmx:afterSwap", function (event) {
			wireRowLinks(event.detail && event.detail.target);
			wireComposer();
			clearTyping();

			/* The server may have OOB-swapped the hidden thread field;
			 * whatever it says now is the conversation to resume. */
			syncThreadFromInput();

			if (event.target.closest && event.target.closest(".chat-log")) {
				scrollChatToBottom();
			}
		});

		document.body.addEventListener("htmx:sseMessage", function () {
			scrollChatToBottom();
		});

		document.body.addEventListener("htmx:responseError", function (event) {
			var status = event.detail && event.detail.status;

			clearTyping();

			/* A 422 carries field-level validation messages that the
			 * server has already rendered into the form, so it should not
			 * also raise a toast. Anything else is worth surfacing. */
			if (status && status !== 422) {
				toast("Request failed (" + status + ")", "negative");
			}
		});

		document.body.addEventListener("htmx:sendError", function () {
			clearTyping();
			toast("Cannot reach the server", "negative", 6000);
		});
	}

	/* ------------------------------------------------------------------ */
	/* Bootstrap                                                           */
	/* ------------------------------------------------------------------ */

	function init() {
		applyTheme(storedTheme());

		document.querySelectorAll("[data-theme-toggle]").forEach(function (el) {
			el.addEventListener("click", cycleTheme);
		});

		document.querySelectorAll("[data-theme-option]").forEach(function (el) {
			el.addEventListener("click", function () {
				setTheme(el.getAttribute("data-theme-option"));
			});
		});

		document.querySelectorAll("[data-ai-toggle]").forEach(function (el) {
			el.addEventListener("click", togglePanel);
		});

		document.querySelectorAll("[data-ai-close]").forEach(function (el) {
			el.addEventListener("click", function () {
				setPanelOpen(false);
			});
		});

		document.querySelectorAll("[data-ai-new]").forEach(function (el) {
			el.addEventListener("click", startNewThread);
		});

		try {
			var stored = window.localStorage.getItem(STORAGE_PANEL);

			if (stored === "1") {
				setPanelOpen(true);
			} else if (stored === "0" && panel()
			           && panel().classList.contains("open")) {
				/* The server rendered it open (ui.chat_dock_expanded)
				 * but this browser last chose to close it. */
				setPanelOpen(false);
			} else if (panel() && panel().classList.contains("open")) {
				/* Rendered open with no stored preference: make the
				 * body class agree so the launcher hides. */
				document.body.classList.add("ai-open");
			}
		} catch (e) {
			/* No stored preference; leave the panel as rendered. */
		}

		resumeThread();
		wireComposer();
		wirePanelResize();

	/*
	 * The kanban board.
	 *
	 * Every card also carries a status select and a Move button, which is
	 * what a browser without scripting uses -- and what this handler posts
	 * to. There is one endpoint and one set of rules; dragging is a nicer
	 * way to reach them, not a second implementation that can disagree.
	 */
	function wireBoard(root) {
		var board = root.querySelector("[data-board]");

		if (!board) {
			return;
		}

		var dragging = null;

		board.addEventListener("dragstart", function (event) {
			var card = event.target.closest("[data-ticket]");

			if (!card) {
				return;
			}

			dragging = card;
			card.classList.add("dragging");
			event.dataTransfer.effectAllowed = "move";
			/* Firefox will not start a drag without payload. */
			event.dataTransfer.setData("text/plain", card.dataset.ticket);
		});

		board.addEventListener("dragend", function () {
			if (dragging) {
				dragging.classList.remove("dragging");
			}

			dragging = null;

			Array.prototype.forEach.call(
				board.querySelectorAll(".drop-target"),
				function (el) { el.classList.remove("drop-target"); }
			);
		});

		board.addEventListener("dragover", function (event) {
			var column = event.target.closest("[data-drop]");

			if (!column || !dragging) {
				return;
			}

			/* Without this the browser refuses the drop. */
			event.preventDefault();
			event.dataTransfer.dropEffect = "move";
			column.classList.add("drop-target");
		});

		board.addEventListener("dragleave", function (event) {
			var column = event.target.closest("[data-drop]");

			if (column && !column.contains(event.relatedTarget)) {
				column.classList.remove("drop-target");
			}
		});

		board.addEventListener("drop", function (event) {
			var column = event.target.closest("[data-drop]");

			if (!column || !dragging) {
				return;
			}

			event.preventDefault();
			column.classList.remove("drop-target");

			var card = dragging;
			var below = event.target.closest("[data-ticket]");
			var body = column;

			/*
			 * Move the card immediately, then tell the server. Waiting
			 * for a round trip to redraw a card the pointer already
			 * dropped feels broken even when it is fast.
			 */
			if (below && below !== card) {
				body.insertBefore(card, below.nextSibling);
			} else {
				body.appendChild(card);
			}

			var previous = card.previousElementSibling;
			var data = new URLSearchParams();

			data.set("status", column.dataset.drop);
			data.set("async", "1");

			if (previous && previous.dataset.ticket) {
				data.set("after", previous.dataset.ticket);
			} else {
				data.set("first", "1");
			}

			/* Keep the fallback select in step, so the card still says
			 * the right thing if scripting is later disabled. */
			var select = card.querySelector(".ticket-move select");

			if (select) {
				select.value = column.dataset.drop;
			}

			updateColumnCounts(board);

			fetch("/tickets/" + card.dataset.ticket + "/move", {
				method: "POST",
				headers: { "Content-Type": "application/x-www-form-urlencoded" },
				body: data.toString(),
				credentials: "same-origin"
			}).then(function (response) {
				if (!response.ok) {
					throw new Error("move failed");
				}
			}).catch(function () {
				/*
				 * The card is already where the pointer left it, and the
				 * server disagrees. Say so rather than silently showing
				 * a board that is wrong.
				 */
				if (window.venture && window.venture.toast) {
					window.venture.toast("Could not move that ticket. Reloading.");
				}

				window.setTimeout(function () { window.location.reload(); }, 1200);
			});
		});
	}

	function updateColumnCounts(board) {
		Array.prototype.forEach.call(
			board.querySelectorAll(".board-column"),
			function (column) {
				var count = column.querySelectorAll("[data-ticket]").length;
				var label = column.querySelector(".count");

				if (label) {
					label.textContent = String(count);
				}
			}
		);
	}

		wireShortcuts();
		wireRowLinks(document);
		wireBoard(document);
		wireServerEvents();
		scrollChatToBottom();
	}

	window.venture = {
		toast: toast,
		openModal: openModal,
		setTheme: setTheme,
		togglePanel: togglePanel,
		scrollChatToBottom: scrollChatToBottom
	};

	if (document.readyState === "loading") {
		document.addEventListener("DOMContentLoaded", init);
	} else {
		init();
	}
}(window, document));
