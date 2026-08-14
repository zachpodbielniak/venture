/*
 * venture.js - VENTURE web UI behaviour
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is presentation: theme, the chat dock, toasts, keyboard
 * shortcuts, small table conveniences. All data movement goes through hx-*
 * attributes rendered by the server, so this file never builds a URL or
 * knows a route.
 */

(function (window, document) {
	"use strict";

	var STORAGE_THEME = "venture.theme";
	var STORAGE_DOCK = "venture.dock.open";

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
	/* AI chat dock                                                        */
	/* ------------------------------------------------------------------ */

	function dock() {
		return document.querySelector(".dock");
	}

	function setDockOpen(open) {
		var el = dock();

		if (!el) {
			return;
		}

		el.classList.toggle("open", open);

		try {
			window.localStorage.setItem(STORAGE_DOCK, open ? "1" : "0");
		} catch (e) {
			/* Not persisting the dock state is not worth failing over. */
		}

		if (open) {
			var input = el.querySelector(".chat-input textarea");

			if (input) {
				input.focus();
			}

			scrollChatToBottom();
		}
	}

	function toggleDock() {
		var el = dock();

		setDockOpen(el ? !el.classList.contains("open") : true);
	}

	function scrollChatToBottom() {
		var log = document.querySelector(".chat-log");

		if (log) {
			log.scrollTop = log.scrollHeight;
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
			 * there is a reliable way out of the dock. */
			if (event.key === "Escape") {
				var open = document.querySelector(".modal-backdrop");

				if (open) {
					open.remove();
					return;
				}

				if (dock() && dock().classList.contains("open")) {
					setDockOpen(false);
					return;
				}
			}

			/* Ctrl+/ toggles the dock from anywhere, typing or not. */
			if (event.key === "/" && (event.ctrlKey || event.metaKey)) {
				event.preventDefault();
				toggleDock();
				return;
			}

			if (isTyping(event.target)) {
				return;
			}

			if (event.key === "/") {
				var search = document.querySelector("[data-search-input]");

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
			["Ctrl + /", "Toggle the AI dock"],
			["t", "Cycle theme"],
			["Esc", "Close dialog or dock"],
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
			      + " — open the AI dock to review", "warning", 8000);
			setDockOpen(true);
		});

		document.body.addEventListener("venture:refresh", function () {
			window.location.reload();
		});

		document.body.addEventListener("htmx:afterSwap", function (event) {
			wireRowLinks(event.detail && event.detail.target);
			wireComposer();

			if (event.target.closest && event.target.closest(".chat-log")) {
				scrollChatToBottom();
			}
		});

		document.body.addEventListener("htmx:sseMessage", function () {
			scrollChatToBottom();
		});

		document.body.addEventListener("htmx:responseError", function (event) {
			var status = event.detail && event.detail.status;

			/* A 422 carries field-level validation messages that the
			 * server has already rendered into the form, so it should not
			 * also raise a toast. Anything else is worth surfacing. */
			if (status && status !== 422) {
				toast("Request failed (" + status + ")", "negative");
			}
		});

		document.body.addEventListener("htmx:sendError", function () {
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

		document.querySelectorAll(".dock-bar").forEach(function (el) {
			el.addEventListener("click", function (event) {
				if (!event.target.closest("button:not([data-dock-toggle])")) {
					toggleDock();
				}
			});
		});

		try {
			if (window.localStorage.getItem(STORAGE_DOCK) === "1") {
				setDockOpen(true);
			}
		} catch (e) {
			/* No stored preference; leave the dock as the server rendered it. */
		}

		wireComposer();
		wireShortcuts();
		wireRowLinks(document);
		wireServerEvents();
		scrollChatToBottom();
	}

	window.venture = {
		toast: toast,
		openModal: openModal,
		setTheme: setTheme,
		toggleDock: toggleDock,
		scrollChatToBottom: scrollChatToBottom
	};

	if (document.readyState === "loading") {
		document.addEventListener("DOMContentLoaded", init);
	} else {
		init();
	}
}(window, document));
