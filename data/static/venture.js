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
	var STORAGE_DRAFT = "venture.ai.draft";
	var STORAGE_LAST = "venture.ai.last";

	/* A send in flight. One at a time: a second question before the
	 * first answer would interleave two replies into one log. */
	var chatBusy = false;
	var chatBusyTimer = null;
	var STORAGE_RECENT = "venture.palette.recent";

	/* The moment a "g" was pressed, for the two-key jumps. */
	var pendingGo = 0;

	/*
	 * What gets a scroll-entry animation: whole content blocks only.
	 *
	 * Deliberately not table rows. Staggering a hundred rows makes a list
	 * unreadable while it plays and, worse, makes it look empty to anyone
	 * scanning it -- which is what a list page is for. Board columns are
	 * in; the cards inside them are not, because a card can also arrive
	 * mid-drag.
	 */
	/*
	 * The assistant's mark, kept byte-identical to VENTURE_SPARK in
	 * venture-web-server.c. This is the optimistic echo of a message the
	 * server then re-renders; if the two disagree the avatar visibly
	 * changes the moment the real response arrives.
	 */
	var SPARK_SVG = "<svg viewBox=\"0 0 24 24\" fill=\"currentColor\" "
		+ "aria-hidden=\"true\" focusable=\"false\">"
		+ "<path d=\"M12 2.5l1.9 6.1 6.1 1.9-6.1 1.9-1.9 6.1-1.9-6.1"
		+ "L4 10.5l6.1-1.9L12 2.5z\"/></svg>";

	var REVEAL_SELECTOR = [
		".main > .card",
		".main > .grid > .card",
		".main > .grid > .stat",
		".main > .bento > *",
		".main > .dash-grid > .card",
		".main > .dash-grid > .dash-card",
		".main > .stat-row > .stat",
		".main > .board > .board-column"
	].join(",");

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

		if (theme === "light" || theme === "dark" || theme === "mocha") {
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

	/*
	 * The configured default (ui.theme) is what applies until the person
	 * picks one; the server hands it over in window.VENTURE_THEME_DEFAULT
	 * from the same inline script that applies it before first paint.
	 */
	function defaultTheme() {
		return window.VENTURE_THEME_DEFAULT || "system";
	}

	function storedTheme() {
		try {
			return window.localStorage.getItem(STORAGE_THEME) || defaultTheme();
		} catch (e) {
			return defaultTheme();
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
		var order = ["system", "light", "dark", "mocha"];
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

		if (open) {
			document.querySelectorAll(".ai-fab.unread").forEach(function (fab) {
				fab.classList.remove("unread");
			});
		}

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

	/*
	 * The starter questions show while the conversation is empty and
	 * make way for it once it exists. Decided from the log itself rather
	 * than from state, so a replayed thread, a fresh one and a deleted
	 * one all get it right without each remembering to say so.
	 */
	function syncStarters() {
		var starters = document.getElementById("chat-starters");
		var log = document.getElementById("chat-log");

		if (!starters || !log) {
			return;
		}

		starters.hidden = !!log.querySelector(".msg, .thread-list");
	}

	function setChatBusy(busy) {
		var form = document.querySelector(".chat-input");
		var send = form && form.querySelector("button[type=submit]");

		chatBusy = busy;

		if (send) {
			send.disabled = busy;
		}

		window.clearTimeout(chatBusyTimer);

		if (busy) {
			/* A reply that never comes must not wedge the composer:
			 * the server's own timeout is well inside this. */
			chatBusyTimer = window.setTimeout(function () {
				setChatBusy(false);
				clearTyping();
			}, 180000);
		}
	}

	function currentPagePath() {
		return window.location.pathname || "/";
	}

	function isRecordPath(path) {
		return /^\/e\/[^/]+\/[1-9][0-9]*$/.test(path);
	}

	function rememberDraft(text) {
		try {
			if (text) {
				window.localStorage.setItem(STORAGE_DRAFT, text);
			} else {
				window.localStorage.removeItem(STORAGE_DRAFT);
			}
		} catch (e) {
			/* The draft lives in the box; it just will not survive
			 * a navigation. */
		}
	}

	function storedDraft() {
		try {
			return window.localStorage.getItem(STORAGE_DRAFT) || "";
		} catch (e) {
			return "";
		}
	}

	function rememberLastQuestion(text) {
		try {
			window.localStorage.setItem(STORAGE_LAST, text);
		} catch (e) {
			/* Then Up will recall nothing. */
		}
	}

	function lastQuestion() {
		try {
			return window.localStorage.getItem(STORAGE_LAST) || "";
		} catch (e) {
			return "";
		}
	}

	/* Puts a question in the composer and sends it, as if typed. */
	function askQuestion(text) {
		var textarea = document.querySelector(".chat-input textarea");
		var form = textarea && textarea.closest("form");

		if (!textarea || !form || !text) {
			return;
		}

		setPanelOpen(true);
		textarea.value = text;
		textarea.style.height = "auto";
		textarea.style.height = textarea.scrollHeight + "px";
		form.dispatchEvent(new Event("submit",
			{ bubbles: true, cancelable: true }));
	}

	/*
	 * Slash commands, for the hands that never leave the keyboard. Only
	 * a handful, and none of them talks to the model: a line starting
	 * with a slash that is not one of these is sent as a question.
	 *
	 * Returns true when the line was a command and has been handled.
	 */
	function runSlashCommand(line) {
		var word = line.trim().split(/\s+/)[0].toLowerCase();

		switch (word) {
		case "/new":
			startNewThread();
			return true;
		case "/threads":
		case "/history":
			if (window.htmx) {
				window.htmx.ajax("GET", "/ui/chat/threads", "#chat-log");
			}
			return true;
		case "/export":
			exportThread();
			return true;
		case "/close":
			setPanelOpen(false);
			return true;
		case "/help":
			toast("Type / for commands, @ to name a record, # for a "
				+ "knowledge base. Up recalls the last question; "
				+ "Shift+Enter is a new line.", "info", 8000);
			return true;
		default:
			return false;
		}
	}

	function exportThread() {
		var id = threadInput() && threadInput().value;

		if (!id) {
			toast("Nothing to export yet: start a conversation first",
			      "info");
			return;
		}

		window.location.href = "/ui/chat/thread/" + encodeURIComponent(id)
			+ "/export";
	}

	/*
	 * Rename from the resume list. A prompt rather than an inline
	 * editor: it is one line, it happens rarely, and the browser's own
	 * dialog needs no markup that could get out of step with the list.
	 */
	function renameThread(id, current) {
		var title = window.prompt("Name this conversation", current || "");

		if (title === null || title.trim() === "") {
			return;
		}

		var data = new URLSearchParams();

		data.set("title", title.trim());

		window.fetch("/ui/chat/thread/" + encodeURIComponent(id) + "/rename", {
			method: "POST",
			headers: { "Content-Type": "application/x-www-form-urlencoded" },
			body: data.toString(),
			credentials: "same-origin"
		}).then(function (response) {
			if (!response.ok) {
				throw new Error("rename failed (" + response.status + ")");
			}

			return response.json();
		}).then(function (body) {
			var row = document.querySelector(
				"[data-thread-item=\"" + body.id + "\"] .thread-title");
			var input = threadInput();
			var heading = document.getElementById("ai-panel-title");

			if (row) {
				row.textContent = body.title;
			}

			if (heading && input && String(input.value) === String(body.id)) {
				heading.textContent = body.title;
			}
		}).catch(function (failure) {
			toast("Could not rename: " + failure.message, "negative");
		});
	}

	/*
	 * A copy button on every reply, added after the fact so the server's
	 * render and the stored replay stay identical. Copies the text as
	 * displayed -- lists as lines, code as code -- not the markup.
	 */
	function wireReplyTools(root) {
		(root || document).querySelectorAll(".msg.ai .msg-content")
			.forEach(function (content) {
				if (content.ventureCopyWired
				    || content.closest(".typing-row")
				    || !content.textContent.trim()) {
					return;
				}

				content.ventureCopyWired = true;

				var button = document.createElement("button");

				button.type = "button";
				button.className = "msg-copy";
				button.title = "Copy this reply";
				button.textContent = "Copy";
				button.addEventListener("click", function () {
					var text = content.innerText.replace(/\n?Copy$/, "");

					if (navigator.clipboard && navigator.clipboard.writeText) {
						navigator.clipboard.writeText(text).then(function () {
							button.textContent = "Copied";
							window.setTimeout(function () {
								button.textContent = "Copy";
							}, 1500);
						}, function () {
							toast("Could not copy", "negative");
						});
					}
				});

				content.appendChild(button);
			});
	}

	function wireChatLogClicks() {
		var log = document.getElementById("chat-log");

		if (!log || log.ventureClicksWired) {
			return;
		}

		log.ventureClicksWired = true;

		/* One listener for the log: the rows are re-rendered on every
		 * swap and would lose per-row handlers. */
		log.addEventListener("click", function (event) {
			var retry = event.target.closest("[data-ai-retry]");
			var rename = event.target.closest("[data-ai-rename]");

			if (retry) {
				var last = lastQuestion();

				if (last) {
					retry.closest(".msg").remove();
					askQuestion(last);
				} else {
					toast("Nothing to retry", "info");
				}

				return;
			}

			if (rename) {
				var row = rename.closest("[data-thread-item]");
				var current = row && row.querySelector(".thread-title");

				renameThread(rename.getAttribute("data-ai-rename"),
				             current ? current.textContent : "");
			}
		});

		log.addEventListener("input", function (event) {
			var filter = event.target.closest("[data-ai-filter]");

			if (!filter) {
				return;
			}

			var needle = filter.value.trim().toLowerCase();

			log.querySelectorAll("[data-thread-item]").forEach(function (row) {
				row.hidden = needle !== ""
					&& row.textContent.toLowerCase().indexOf(needle) === -1;
			});
		});
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

		syncStarters();

		if (title) {
			title.textContent = "Ask VENTURE";
		}

		var composer = document.querySelector(".chat-input textarea");

		if (composer) {
			composer.focus();
		}
	}

	/* ------------------------------------------------------------------ */
	/* Streaming replies                                                   */
	/* ------------------------------------------------------------------ */

	/*
	 * The server answers a streamed turn with an empty bubble naming the
	 * stream that will fill it. Opening that stream is this; what comes
	 * down it is prose, a line of status while the model is off calling
	 * a tool, and finally the whole reply rendered the way a stored one
	 * is -- markdown, links, approval cards -- which replaces everything
	 * the deltas painted.
	 *
	 * The deltas are put in with textContent, never innerHTML: they are
	 * the model's words arriving a fragment at a time, and a fragment
	 * cannot be escaped correctly on its own even if it were worth
	 * trying.
	 */
	function wireChatStream(root) {
		(root || document).querySelectorAll("[data-chat-stream]")
			.forEach(function (bubble) {
				if (bubble.ventureStreamBound || !window.EventSource) {
					return;
				}

				bubble.ventureStreamBound = true;
				openChatStream(bubble);
			});
	}

	function openChatStream(bubble) {
		var url = bubble.getAttribute("data-chat-stream");
		var text = bubble.querySelector("[data-chat-text]");
		var status = bubble.querySelector("[data-chat-status]");
		var source = new EventSource(url, { withCredentials: true });
		var finished = false;

		/*
		 * The finished reply, or the failure, in place of the bubble.
		 * Inserted before it and then the bubble removed, because the
		 * server sends several top-level nodes when the turn staged a
		 * change: the message, and a card per change to approve.
		 */
		function finish(html) {
			var holder = document.createElement("div");
			var parent = bubble.parentNode;

			finished = true;
			source.close();

			if (!parent) {
				setChatBusy(false);
				return;
			}

			holder.innerHTML = html;

			while (holder.firstChild) {
				parent.insertBefore(holder.firstChild, bubble);
			}

			parent.removeChild(bubble);

			/* The approval buttons are hx-post; nothing has wired
			 * them yet, because this markup never went through a
			 * swap. */
			if (window.htmx) {
				window.htmx.process(parent);
			}

			wireReplyTools(parent);
			setChatBusy(false);
			syncStarters();
			scrollChatToBottom();
			notifyIfPanelClosed();
		}

		source.addEventListener("delta", function (event) {
			if (status) {
				status.textContent = "";
			}

			if (text) {
				text.textContent += event.data;
			}

			scrollChatToBottom();
		});

		source.addEventListener("status", function (event) {
			if (status) {
				status.textContent = event.data.trim();
			}

			scrollChatToBottom();
		});

		source.addEventListener("done", function (event) {
			finish(event.data);
		});

		/*
		 * EventSource raises this both when a connection fails and
		 * when the server closes one, and it reconnects on its own
		 * unless told otherwise. The token is spent, so a reconnect
		 * would only ask for a turn that is no longer there --
		 * closing it here is what stops that becoming a loop.
		 */
		source.addEventListener("error", function () {
			if (finished) {
				return;
			}

			finished = true;
			source.close();

			if (status) {
				status.textContent = "";
			}

			var content = bubble.querySelector(".msg-content");
			var notice = document.createElement("div");
			var retry = document.createElement("div");
			var again = document.createElement("button");

			/* Built as nodes rather than markup. Half of this used to
			 * be an innerHTML string, which put the class name of a
			 * failure notice into the script inlined on every page --
			 * and a test looking for one on a dashboard found it. */
			notice.className = "notice negative";

			if (text && text.textContent !== "") {
				/* Half an answer is still worth keeping; what is
				 * missing is said underneath it. */
				notice.textContent = "The connection dropped before "
					+ "the answer finished.";
				content.appendChild(notice);
			} else {
				notice.textContent = "The assistant could not be reached.";
				content.textContent = "";
				content.appendChild(notice);
			}

			again.type = "button";
			again.className = "btn btn-sm";
			again.setAttribute("data-ai-retry", "");
			again.textContent = "Try again";
			retry.className = "chat-retry";
			retry.appendChild(again);
			content.appendChild(retry);
			bubble.classList.remove("streaming");

			setChatBusy(false);
			scrollChatToBottom();
		});
	}

	/* A reply that landed while the panel was closed is easy to miss. */
	function notifyIfPanelClosed() {
		if (!panel() || panel().classList.contains("open")) {
			return;
		}

		document.querySelectorAll(".ai-fab").forEach(function (fab) {
			fab.classList.add("unread");
		});
		toast("The assistant replied \u2014 Ctrl+/ to read it", "info", 6000);
	}

	/* ------------------------------------------------------------------ */
	/* The composer's menus: # for a knowledge base, / for a skill        */
	/* ------------------------------------------------------------------ */

	/*
	 * One popup for both. It watches the word at the caret: a word that
	 * starts with # offers the knowledge bases, and a / at the very
	 * start of the box offers the commands and the skills. What it
	 * offers comes from /ui/chat/complete, fetched once and kept for a
	 * minute, so opening the menu costs nothing after the first time.
	 * Arrow keys move, Enter or Tab picks, Escape closes; a click picks
	 * too. The commands are the ones the composer already understands,
	 * so the menu can only offer what typing could do.
	 */
	var completeState = null;
	var completeTimer = null;

	/*
	 * The commands the composer runs itself. They are listed here and
	 * not on the server because the script is what carries them out, and
	 * a menu offering one the script did not understand would be a menu
	 * that lies.
	 */
	var CLIENT_COMMANDS = [
		{ trigger: "new", name: "New conversation", description: "Start fresh" },
		{ trigger: "threads", name: "Conversations",
		  description: "Resume an earlier one" },
		{ trigger: "export", name: "Export",
		  description: "Download this conversation as org" },
		{ trigger: "close", name: "Close", description: "Hide the panel" },
		{ trigger: "help", name: "Help",
		  description: "What the composer understands" }
	];

	/*
	 * Where the token under the caret starts, by the same rule the
	 * harness applies: a slash only at the very start of the composer, an
	 * @ or a # at the start of a word, and no spaces inside.
	 *
	 * The server sends its own range back, in bytes. This is measured in
	 * the units the textarea actually slices by, which is the only way a
	 * question with an accent in it replaces the right characters.
	 */
	function completionRange(textarea) {
		var caret = textarea.selectionStart;
		var before = textarea.value.slice(0, caret);
		var slash = /^\/([^\s]*)$/.exec(before);
		var token = /(^|\s)([@#])([^\s]*)$/.exec(before);

		if (slash) {
			return { sigil: "/", start: 0, end: caret };
		}

		if (token) {
			return { sigil: token[2], start: caret - token[3].length - 1,
			         end: caret };
		}

		return null;
	}

	function completeMenu() {
		var menu = document.getElementById("chat-complete");

		if (!menu) {
			var form = document.querySelector(".chat-input");

			if (!form) {
				return null;
			}

			menu = document.createElement("div");
			menu.id = "chat-complete";
			menu.className = "chat-complete";
			menu.hidden = true;
			menu.setAttribute("role", "listbox");
			form.parentNode.insertBefore(menu, form);
		}

		return menu;
	}

	function closeCompletion() {
		var menu = document.getElementById("chat-complete");

		completeState = null;
		window.clearTimeout(completeTimer);

		if (menu) {
			menu.hidden = true;
			menu.innerHTML = "";
		}
	}

	function renderCompletion(textarea, range, kind, items) {
		var menu = completeMenu();

		if (!menu) {
			return;
		}

		/* The panel's own commands, filtered the way the server filtered
		 * its own: by what has been typed after the slash. */
		if (kind === "command") {
			var typed = textarea.value.slice(range.start + 1,
			                                range.end).toLowerCase();

			CLIENT_COMMANDS.forEach(function (command) {
				if (typed !== ""
				    && command.trigger.indexOf(typed) !== 0) {
					return;
				}

				items.push({ insert: "/" + command.trigger,
				             label: "/" + command.trigger,
				             name: command.name,
				             description: command.description,
				             origin: "panel", command: true });
			});

			items.push({ insert: "", label: "Harness\u2026", name: "",
			             description: "Every command, and where it came from",
			             origin: "", href: "/harness" });
		}

		if (items.length === 0) {
			closeCompletion();
			return;
		}

		completeState = { textarea: textarea, range: range, items: items,
		                  index: 0 };
		menu.innerHTML = "";

		items.forEach(function (item, i) {
			var row = document.createElement("div");
			var label = document.createElement("span");
			var name = document.createElement("span");
			var desc = document.createElement("span");
			var origin = document.createElement("span");

			row.className = "chat-complete-item" + (i === 0 ? " active" : "");
			row.setAttribute("role", "option");
			label.className = "chat-complete-label";
			label.textContent = item.label;
			name.className = "chat-complete-name";
			name.textContent = item.name || "";
			desc.className = "chat-complete-desc";
			desc.textContent = item.description || "";
			origin.className = "chat-complete-group";
			origin.textContent = item.origin || "";
			row.appendChild(label);
			row.appendChild(name);
			row.appendChild(desc);
			row.appendChild(origin);
			row.addEventListener("mousedown", function (event) {
				/* mousedown, so the composer keeps focus. */
				event.preventDefault();
				pickCompletion(i);
			});
			menu.appendChild(row);
		});

		menu.hidden = false;
	}

	function moveCompletion(delta) {
		var menu = document.getElementById("chat-complete");

		if (!completeState || !menu) {
			return;
		}

		var count = completeState.items.length;

		completeState.index = (completeState.index + delta + count) % count;

		menu.querySelectorAll(".chat-complete-item").forEach(function (row, i) {
			row.classList.toggle("active", i === completeState.index);

			if (i === completeState.index && row.scrollIntoView) {
				row.scrollIntoView({ block: "nearest" });
			}
		});
	}

	function pickCompletion(index) {
		if (!completeState) {
			return;
		}

		var state = completeState;
		var item = state.items[index === undefined ? state.index : index];
		var textarea = state.textarea;

		closeCompletion();

		if (!item) {
			return;
		}

		if (item.href) {
			window.location.href = item.href;
			return;
		}

		var value = textarea.value;

		textarea.value = value.slice(0, state.range.start) + item.insert
			+ value.slice(state.range.end);

		var caret = state.range.start + item.insert.length;

		textarea.setSelectionRange(caret, caret);
		textarea.style.height = "auto";
		textarea.style.height = textarea.scrollHeight + "px";
		rememberDraft(textarea.value);

		/*
		 * A command the panel runs goes at once. Everything else --
		 * a skill, a record, a base -- is part of a sentence that is
		 * not finished yet, and the menu reopens if the next thing
		 * typed is completable too.
		 */
		if (item.command) {
			runSlashCommand(item.insert);
			textarea.value = "";
			textarea.style.height = "auto";
			rememberDraft("");
			return;
		}

		updateCompletion(textarea);
	}

	/*
	 * What can be completed is the server's answer, not a guess here: it
	 * knows the commands on disk, the record types this person may read,
	 * and the knowledge bases. Debounced, because "@ticket/pay" is four
	 * searches if every keystroke asks.
	 */
	function updateCompletion(textarea) {
		var range = completionRange(textarea);

		if (!range) {
			closeCompletion();
			return;
		}

		window.clearTimeout(completeTimer);
		completeTimer = window.setTimeout(function () {
			var buffer = textarea.value;
			var cursor = textarea.selectionStart;
			var query = "?buffer=" + encodeURIComponent(buffer)
				+ "&cursor=" + encodeURIComponent(String(cursor));

			window.fetch("/ui/chat/complete" + query, {
				credentials: "same-origin"
			}).then(function (response) {
				return response.ok ? response.json() : null;
			}).then(function (body) {
				/* The caret may have moved on while this was in
				 * flight; what came back is about where it was. */
				var now = completionRange(textarea);

				if (!body || !now || now.sigil !== range.sigil
				    || textarea.value !== buffer) {
					return;
				}

				renderCompletion(textarea, now, body.kind,
				                 body.items || []);
			}).catch(function () {
				/* A menu is a convenience; losing it must not make
				 * the composer feel broken. */
			});
		}, 120);
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

		/* A half-typed question survives a click on a link. */
		if (textarea.value === "" && storedDraft()) {
			textarea.value = storedDraft();
		}

		textarea.addEventListener("input", function () {
			textarea.style.height = "auto";
			textarea.style.height = textarea.scrollHeight + "px";
			rememberDraft(textarea.value);
			updateCompletion(textarea);
		});

		textarea.addEventListener("blur", function () {
			/* After the click a menu row may be taking. */
			window.setTimeout(closeCompletion, 150);
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

				/*
				 * Before the hx runtime sees the submit: a second
				 * send while one is in flight is dropped, and a
				 * slash command never leaves the browser. Capture
				 * phase, so this runs first whatever order the
				 * runtime wired itself in.
				 */
				form.addEventListener("submit", function (event) {
					var text = textarea.value.trim();

					if (text.charAt(0) === "/" && runSlashCommand(text)) {
						event.preventDefault();
						event.stopImmediatePropagation();
						textarea.value = "";
						textarea.style.height = "auto";
						rememberDraft("");
						return;
					}

					if (chatBusy) {
						event.preventDefault();
						event.stopImmediatePropagation();
						toast("Still answering the last one", "info", 2500);
						return;
					}

					if (text === "") {
						event.preventDefault();
						event.stopImmediatePropagation();
						return;
					}

					/* The page the question is about rides along;
					 * the server turns it into what the model sees. */
					var context = document.getElementById("chat-context");

					if (context) {
						context.value = currentPagePath();
					}

					/*
					 * And whether this browser can watch the answer
					 * being written. Asked for per send rather than
					 * once, so a form the server rendered before the
					 * script ran is never told the browser can do
					 * something it cannot.
					 */
					var stream = document.getElementById("chat-stream");

					if (stream) {
						stream.value = window.EventSource ? "1" : "";
					}

					setChatBusy(true);
					rememberLastQuestion(text);
					rememberDraft("");
					closeCompletion();
				}, true);

				form.addEventListener("submit", function () {
					var text = textarea.value.trim();
					var log = document.getElementById("chat-log");

					if (text !== "" && log) {
						/* The echo names the attachments the same
						 * way the stored transcript will, so the
						 * optimistic render and next week's replay
						 * are identical. */
						var echo = text;

						pendingAttachments().forEach(function (chip) {
							echo += "\n[Attached: " + chip.dataset.name
								+ " (document #" + chip.dataset.docId
								+ ")]";
						});

						if (isRecordPath(currentPagePath())) {
							echo += "\n[While viewing "
								+ currentPagePath() + "]";
						}

						appendUserEcho(log, echo);
						syncStarters();
						appendTyping(log);
						scrollChatToBottom();
					}

					window.setTimeout(function () {
						textarea.value = "";
						textarea.style.height = "auto";
						clearAttachments();
					}, 0);
				});
			}
		}

		textarea.addEventListener("keydown", function (event) {
			/* The menu, while it is open, owns these keys. */
			if (completeState) {
				if (event.key === "ArrowDown") {
					event.preventDefault();
					moveCompletion(1);
					return;
				}

				if (event.key === "ArrowUp") {
					event.preventDefault();
					moveCompletion(-1);
					return;
				}

				if (event.key === "Enter" || event.key === "Tab") {
					event.preventDefault();
					pickCompletion();
					return;
				}

				if (event.key === "Escape") {
					event.preventDefault();
					event.stopPropagation();
					closeCompletion();
					return;
				}
			}

			if (event.key === "Enter" && !event.shiftKey) {
				event.preventDefault();

				var form = textarea.closest("form");

				if (form && textarea.value.trim() !== "") {
					form.dispatchEvent(new Event("submit",
						{ bubbles: true, cancelable: true }));
				}

				return;
			}

			/* Up in an empty box recalls the last question, to fix a
			 * word and send it again. */
			if (event.key === "ArrowUp" && textarea.value === ""
			    && lastQuestion()) {
				event.preventDefault();
				textarea.value = lastQuestion();
				textarea.style.height = "auto";
				textarea.style.height = textarea.scrollHeight + "px";
				textarea.setSelectionRange(textarea.value.length,
				                           textarea.value.length);
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
		/* Must match VENTURE_SPARK in venture-web-server.c: this is the
		 * optimistic echo, and the server re-renders the same message. */
		row.innerHTML = "<span class=\"msg-avatar\">" + SPARK_SVG + "</span>"
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

	/* ------------------------------------------------------------------ */
	/* Chat attachments                                                    */
	/* ------------------------------------------------------------------ */

	function attachIdsInput() {
		return document.getElementById("chat-attach-ids");
	}

	function pendingAttachments() {
		return Array.prototype.slice.call(
			document.querySelectorAll("#chat-attachments .attach-chip"));
	}

	function syncAttachIds() {
		var input = attachIdsInput();

		if (input) {
			input.value = pendingAttachments().map(function (chip) {
				return chip.dataset.docId;
			}).join(",");
		}
	}

	function addAttachChip(id, name, textChars, isImage, file) {
		var box = document.getElementById("chat-attachments");
		var chip = document.createElement("span");
		var label = document.createElement("span");
		var remove = document.createElement("button");

		if (!box) {
			return;
		}

		chip.className = "attach-chip";
		chip.dataset.docId = String(id);
		chip.dataset.name = name;

		/*
		 * A thumbnail from the local file, not a round trip: the bytes
		 * are already in the browser, and a screenshot you can see is
		 * how you notice you attached the wrong window.
		 */
		if (isImage && file && window.URL && window.URL.createObjectURL) {
			var thumb = document.createElement("img");

			thumb.className = "attach-thumb";
			thumb.alt = "";
			thumb.src = window.URL.createObjectURL(file);
			thumb.addEventListener("load", function () {
				window.URL.revokeObjectURL(thumb.src);
			});
			chip.appendChild(thumb);
		}

		label.className = "attach-name";
		label.textContent = name;
		chip.appendChild(label);

		/*
		 * A file whose text could not be extracted still attaches -- the
		 * record is kept -- but the operator should know the model will
		 * not be able to read it. An image needs no text: the model
		 * reads the picture itself.
		 */
		if (!textChars && !isImage) {
			var warn = document.createElement("span");

			warn.className = "attach-warn";
			warn.textContent = "no text";
			warn.title = "No text could be extracted; the AI cannot "
				+ "read this file's contents";
			chip.appendChild(warn);
		}

		remove.type = "button";
		remove.className = "attach-remove";
		remove.textContent = "×";
		remove.title = "Remove attachment";
		remove.addEventListener("click", function () {
			chip.remove();
			syncAttachIds();
		});
		chip.appendChild(remove);

		box.appendChild(chip);
		syncAttachIds();
	}

	function clearAttachments() {
		var box = document.getElementById("chat-attachments");

		if (box) {
			box.innerHTML = "";
		}

		syncAttachIds();
	}

	function uploadAttachment(file) {
		var data = new FormData();

		data.append("file", file, file.name);

		return window.fetch("/ui/chat/upload", {
			method: "POST",
			body: data,
			credentials: "same-origin"
		}).then(function (response) {
			if (!response.ok) {
				return response.json().then(function (body) {
					throw new Error((body && body.error &&
						body.error.message) || "upload failed");
				}, function () {
					throw new Error("upload failed ("
						+ response.status + ")");
				});
			}

			return response.json();
		}).then(function (body) {
			addAttachChip(body.id, body.name, body.text_chars,
			              body.is_image, file);
		}).catch(function (failure) {
			toast("Could not attach " + file.name + ": "
				+ failure.message, "negative", 6000);
		});
	}

	function wireAttachments() {
		var button = document.querySelector("[data-ai-attach]");
		var input = document.getElementById("chat-attach-file");

		if (!button || !input || button.ventureWired) {
			return;
		}

		button.ventureWired = true;

		button.addEventListener("click", function () {
			input.click();
		});

		input.addEventListener("change", function () {
			Array.prototype.forEach.call(input.files, uploadAttachment);
			/* Selecting the same file again later must re-trigger
			 * change. */
			input.value = "";
		});

		/*
		 * Paste a screenshot straight into the composer. A screen
		 * capture lands on the clipboard, not on disk, and making the
		 * operator save it to a file first just to attach it is the
		 * step worth removing.
		 */
		{
			var composer = document.querySelector(".chat-input textarea");

			if (composer) {
				composer.addEventListener("paste", function (event) {
					var items = event.clipboardData
						&& event.clipboardData.items;
					var i;

					if (!items) {
						return;
					}

					for (i = 0; i < items.length; i++) {
						if (items[i].kind !== "file"
						    || items[i].type.indexOf("image/") !== 0) {
							continue;
						}

						var file = items[i].getAsFile();

						if (file) {
							/* Pasted captures are all named
							 * "image.png"; the time tells them
							 * apart in the document list. */
							event.preventDefault();
							uploadAttachment(new File([file],
								"pasted-" + Date.now() + "."
								+ (file.type.split("/")[1] || "png"),
								{ type: file.type }));
						}
					}
				});
			}
		}
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
	/* Pickers: a themed select, and searching for a record               */
	/* ------------------------------------------------------------------ */

	/*
	 * A native select draws its popup with the operating system, which
	 * is the one part of this interface no stylesheet reaches: on a dark
	 * theme it opens as a grey slab in somebody else's font, and with
	 * thirty record types in it there is nothing to type into.
	 *
	 * So the select stays, hidden, and keeps being what the form posts
	 * and what every existing handler reads. In front of it goes a
	 * button and a panel drawn from the same tokens as everything else,
	 * with a filter box once there are enough options to lose one in.
	 * Without scripting the native control is simply still there.
	 */
	var PICKER_SEARCH_THRESHOLD = 7;

	var openPicker = null;

	/*
	 * Floating panels are positioned against the viewport, not against
	 * the thing that opened them.
	 *
	 * A card clips its contents -- it has to, or a table's corners
	 * escape it -- so a panel laid out inside one is cut off at the
	 * card's edge. Measured once on open: the type list showed two of
	 * sixty options. Fixed positioning leaves every clipping context
	 * behind, at the cost of having to do the arithmetic here.
	 */
	function placeFloating(panel, anchor) {
		var rect = anchor.getBoundingClientRect();
		var room = window.innerHeight - rect.bottom;

		panel.style.position = "fixed";
		panel.style.left = Math.round(rect.left) + "px";
		panel.style.minWidth = Math.round(rect.width) + "px";

		/* Below when there is room for a usable list, above when the
		 * control is near the bottom of the window. */
		if (room < 180 && rect.top > room) {
			panel.style.top = "auto";
			panel.style.bottom = Math.round(window.innerHeight - rect.top + 2)
				+ "px";
			panel.style.maxHeight = Math.round(rect.top - 12) + "px";
		} else {
			panel.style.bottom = "auto";
			panel.style.top = Math.round(rect.bottom + 2) + "px";
			panel.style.maxHeight = Math.round(room - 12) + "px";
		}
	}

	function closePicker() {
		if (!openPicker) {
			return;
		}

		openPicker.wrap.classList.remove("open");
		openPicker.panel.hidden = true;
		openPicker = null;
	}

	function pickerLabel(select) {
		var option = select.options[select.selectedIndex];

		return option ? option.textContent : "";
	}

	function wirePickers(root) {
		(root || document).querySelectorAll("select").forEach(function (select) {
			if (select.venturePicker || select.multiple
			    || select.hasAttribute("data-no-picker")) {
				return;
			}

			select.venturePicker = true;
			buildPicker(select);
		});
	}

	function buildPicker(select) {
		var wrap = document.createElement("span");
		var button = document.createElement("button");
		var panel = document.createElement("div");
		var filter = document.createElement("input");
		var list = document.createElement("div");
		var cursor = 0;

		wrap.className = "picker";
		button.type = "button";
		button.className = "picker-button";
		button.setAttribute("aria-haspopup", "listbox");
		panel.className = "picker-panel";
		panel.hidden = true;
		filter.type = "search";
		filter.className = "picker-filter";
		filter.placeholder = "Filter\u2026";
		filter.setAttribute("aria-label", "Filter options");
		list.className = "picker-list";
		list.setAttribute("role", "listbox");

		if (select.options.length > PICKER_SEARCH_THRESHOLD) {
			panel.appendChild(filter);
		}

		panel.appendChild(list);

		/* The select keeps its place in the form and its name; it is
		 * only taken out of the tab order and out of sight. */
		select.parentNode.insertBefore(wrap, select);
		wrap.appendChild(select);
		wrap.appendChild(button);
		wrap.appendChild(panel);
		select.classList.add("picker-native");
		select.tabIndex = -1;

		function syncButton() {
			button.textContent = pickerLabel(select);
		}

		function rows() {
			return Array.prototype.slice.call(
				list.querySelectorAll(".picker-option:not([hidden])"));
		}

		function highlight(index) {
			var visible = rows();

			if (visible.length === 0) {
				return;
			}

			cursor = Math.max(0, Math.min(index, visible.length - 1));

			visible.forEach(function (row, i) {
				row.classList.toggle("active", i === cursor);

				if (i === cursor && row.scrollIntoView) {
					row.scrollIntoView({ block: "nearest" });
				}
			});
		}

		function choose(row) {
			select.selectedIndex = parseInt(row.dataset.index, 10);
			syncButton();
			closePicker();
			button.focus();

			/* Everything already listening to this select -- the widget
			 * editor, the board's move form, the bulk bar -- hears the
			 * same event it would have heard from the native one. */
			select.dispatchEvent(new Event("change", { bubbles: true }));
		}

		function build() {
			list.innerHTML = "";

			Array.prototype.forEach.call(select.options, function (option, i) {
				var row = document.createElement("div");

				row.className = "picker-option"
					+ (i === select.selectedIndex ? " selected" : "");
				row.setAttribute("role", "option");
				row.dataset.index = String(i);
				row.textContent = option.textContent;
				row.addEventListener("mousedown", function (event) {
					event.preventDefault();
					choose(row);
				});
				list.appendChild(row);
			});
		}

		function applyFilter() {
			var needle = filter.value.trim().toLowerCase();

			list.querySelectorAll(".picker-option").forEach(function (row) {
				row.hidden = needle !== ""
					&& row.textContent.toLowerCase().indexOf(needle) === -1;
			});

			highlight(0);
		}

		function open() {
			if (select.disabled) {
				return;
			}

			closePicker();
			build();
			filter.value = "";
			applyFilter();
			panel.hidden = false;
			placeFloating(panel, button);
			wrap.classList.add("open");
			openPicker = { wrap: wrap, panel: panel };

			var chosen = rows().findIndex(function (row) {
				return parseInt(row.dataset.index, 10) === select.selectedIndex;
			});

			highlight(chosen < 0 ? 0 : chosen);

			if (panel.contains(filter)) {
				filter.focus();
			}
		}

		button.addEventListener("click", function () {
			if (openPicker && openPicker.wrap === wrap) {
				closePicker();
			} else {
				open();
			}
		});

		filter.addEventListener("input", applyFilter);

		wrap.addEventListener("keydown", function (event) {
			if (panel.hidden) {
				if (event.key === "ArrowDown" || event.key === "Enter"
				    || event.key === " ") {
					event.preventDefault();
					open();
				}

				return;
			}

			if (event.key === "ArrowDown") {
				event.preventDefault();
				highlight(cursor + 1);
			} else if (event.key === "ArrowUp") {
				event.preventDefault();
				highlight(cursor - 1);
			} else if (event.key === "Enter") {
				event.preventDefault();

				var visible = rows();

				if (visible[cursor]) {
					choose(visible[cursor]);
				}
			} else if (event.key === "Escape") {
				event.preventDefault();
				closePicker();
				button.focus();
			}
		});

		/* A change made to the select by anything else -- a form reset,
		 * a script setting a value -- must show on the button. */
		select.addEventListener("change", syncButton);

		syncButton();
	}

	/*
	 * Searching for the record to link to.
	 *
	 * The form posts a numeric id and always did. What this puts in
	 * front of it is a box that searches the type chosen beside it, so
	 * the id is picked from a list of real records rather than copied
	 * out of another tab and hopefully typed correctly.
	 */
	function wireRecordPickers(root) {
		(root || document).querySelectorAll("[data-record-pick]")
			.forEach(function (host) {
				if (host.ventureWired) {
					return;
				}

				host.ventureWired = true;
				buildRecordPicker(host);
			});
	}

	function buildRecordPicker(host) {
		var hidden = host.querySelector("input[type=number]");
		var form = host.closest("form");
		var typeName = host.getAttribute("data-type-field");
		var typeSelect = form && typeName
			? form.querySelector("[name=\"" + typeName + "\"]")
			: null;
		var search = document.createElement("input");
		var results = document.createElement("div");
		var chosen = document.createElement("span");
		var timer = null;

		if (!hidden) {
			return;
		}

		search.type = "search";
		search.className = "record-search";
		search.placeholder = "Search\u2026";
		search.setAttribute("aria-label", "Search for a record to link");
		results.className = "record-results";
		results.hidden = true;
		chosen.className = "record-chosen";
		chosen.hidden = true;

		/*
		 * The number box is what the form posts and stays in the DOM,
		 * but it is no longer something to type into: the search takes
		 * a number as readily as a name, so two boxes for one value
		 * was only ever a question about which of them counted.
		 */
		hidden.classList.add("record-id");
		host.classList.add("searchable");
		host.appendChild(search);
		host.appendChild(chosen);
		host.appendChild(results);

		function clearChoice() {
			hidden.value = "";
			chosen.hidden = true;
			chosen.textContent = "";
		}

		function pick(item) {
			hidden.value = String(item.id);
			chosen.textContent = "#" + item.id + " " + item.label;
			chosen.hidden = false;
			search.value = "";
			results.hidden = true;
			results.innerHTML = "";
		}

		function render(items) {
			results.innerHTML = "";

			if (items.length === 0) {
				var empty = document.createElement("div");

				empty.className = "record-result muted";
				empty.textContent = "Nothing matches";
				results.appendChild(empty);
				placeFloating(results, search);
				results.hidden = false;
				return;
			}

			items.forEach(function (item) {
				var row = document.createElement("div");
				var id = document.createElement("span");

				row.className = "record-result";
				id.className = "record-result-id";
				id.textContent = "#" + item.id;
				row.appendChild(id);
				row.appendChild(document.createTextNode(item.label));
				row.addEventListener("mousedown", function (event) {
					event.preventDefault();
					pick(item);
				});
				results.appendChild(row);
			});

			placeFloating(results, search);
			results.hidden = false;
		}

		function search_now() {
			var type = typeSelect ? typeSelect.value : "";
			var text = search.value.trim();

			if (!type) {
				return;
			}

			window.fetch("/ui/records/search?type="
				+ encodeURIComponent(type) + "&q=" + encodeURIComponent(text), {
				credentials: "same-origin"
			}).then(function (response) {
				return response.ok ? response.json() : { items: [] };
			}).then(function (body) {
				render(body.items || []);
			}).catch(function () {
				results.hidden = true;
			});
		}

		search.addEventListener("input", function () {
			clearChoice();
			window.clearTimeout(timer);
			timer = window.setTimeout(search_now, 160);
		});

		search.addEventListener("focus", search_now);

		search.addEventListener("blur", function () {
			window.setTimeout(function () {
				results.hidden = true;
			}, 150);
		});

		/* A different type is a different set of records; whatever was
		 * chosen under the old one is not it. */
		if (typeSelect) {
			typeSelect.addEventListener("change", function () {
				clearChoice();
				results.hidden = true;
				results.innerHTML = "";
			});
		}
	}

	/* ------------------------------------------------------------------ */
	/* The automation rules editor                                         */
	/* ------------------------------------------------------------------ */

	/*
	 * Line numbers and as-you-type diagnostics for the pod DSL. The
	 * diagnostics come from the server's real parser -- the same one that
	 * loads the file -- so an error shown here is an error that would
	 * have broken the reload, caught while it is still a keystroke away
	 * from fixed.
	 */
	function wirePodEditor() {
		var form = document.querySelector("[data-pod-editor]");

		if (!form || form.ventureEditorWired) {
			return;
		}

		form.ventureEditorWired = true;

		var textarea = form.querySelector("textarea.code-input");
		var gutter = form.querySelector(".code-gutter");
		var status = document.getElementById("pod-diagnostics");
		var url = status && status.dataset.validateUrl;
		var timer = null;

		function renumber() {
			if (!gutter) {
				return;
			}

			var count = textarea.value.split("\n").length;
			var numbers = [];

			for (var i = 1; i <= count; i++) {
				numbers.push(i);
			}

			gutter.textContent = numbers.join("\n");
		}

		function markLine(line) {
			/* The gutter is plain text; the marked line becomes the
			 * one element in it. */
			if (!gutter || !line) {
				return;
			}

			var count = textarea.value.split("\n").length;
			var html = "";

			for (var i = 1; i <= count; i++) {
				html += (i === line)
					? "<span class=\"bad-line\">" + i + "</span>\n"
					: i + "\n";
			}

			gutter.innerHTML = html;
		}

		function validate() {
			if (!url || !status) {
				return;
			}

			var data = new URLSearchParams();

			data.set("source", textarea.value);

			window.fetch(url, {
				method: "POST",
				headers: { "Content-Type":
					"application/x-www-form-urlencoded" },
				body: data.toString(),
				credentials: "same-origin"
			}).then(function (response) {
				return response.json();
			}).then(function (body) {
				if (body.ok) {
					/* .editor-status.ok already carries the colour; a dingbat
					 * check on top of it renders in whatever font the browser
					 * falls back to for U+2713, which is rarely this one. */
					status.textContent = "Parses cleanly.";
					status.className = "editor-status ok";
					renumber();
				} else {
					status.textContent = body.message
						|| "Does not parse.";
					status.className = "editor-status bad";

					var match = /line\s+(\d+)/i.exec(
						body.message || "");

					if (match) {
						markLine(parseInt(match[1], 10));
					}
				}
			}).catch(function () {
				/* Validation is advice; losing it must not make
				 * the editor feel broken. */
			});
		}

		textarea.addEventListener("input", function () {
			renumber();
			window.clearTimeout(timer);
			timer = window.setTimeout(validate, 700);
		});

		textarea.addEventListener("scroll", function () {
			if (gutter) {
				gutter.scrollTop = textarea.scrollTop;
			}
		});

		/* Tab inserts a tab; losing focus to the next button is not
		 * what anybody editing code means by it. */
		textarea.addEventListener("keydown", function (event) {
			if (event.key === "Tab") {
				event.preventDefault();

				var start = textarea.selectionStart;
				var end = textarea.selectionEnd;

				textarea.value = textarea.value.slice(0, start)
					+ "\t" + textarea.value.slice(end);
				textarea.selectionStart = start + 1;
				textarea.selectionEnd = start + 1;
				renumber();
			}
		});

		renumber();

		if (textarea.value.trim() !== "") {
			validate();
		}
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

				if (openPicker) {
					closePicker();
					return;
				}

				if (paletteState) {
					closePalette();
					return;
				}

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

			/* Ctrl+K opens the palette from anywhere, like every other
			 * tool with a command bar. The sidebar search box stays for
			 * a browser without scripting. */
			if (event.key === "k" && (event.ctrlKey || event.metaKey)) {
				event.preventDefault();
				openPalette();
				return;
			}

			if (isTyping(event.target)) {
				return;
			}

			/* Two-key jumps: g then a letter, within a second. */
			if (pendingGo && Date.now() - pendingGo < 1000) {
				var jumps = {
					i: "/inbox", t: "/tickets", d: "/", r: "/runs",
					s: "/sprints", f: "/factory", v: "/views",
					b: "/dashboards"
				};

				pendingGo = 0;

				if (jumps[event.key]) {
					event.preventDefault();
					window.location.href = jumps[event.key];
					return;
				}
			}

			if (event.key === "g") {
				pendingGo = Date.now();
				return;
			}

			/* c makes a new one of whatever the page lists. */
			if (event.key === "c") {
				var create = document.querySelector(
					".page-actions a.btn-primary[href$=\"/new\"]");

				if (create) {
					event.preventDefault();
					window.location.href = create.getAttribute("href");
				}

				return;
			}

			if (event.key === "j" || event.key === "k" || event.key === "x"
			    || event.key === "Enter") {
				if (moveListCursor(event.key)) {
					event.preventDefault();
				}

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
			["Ctrl + K", "Command palette: pages, records, new\u2026"],
			["/", "Focus search"],
			["g i", "Inbox"],
			["g t", "Tickets"],
			["g r", "Runs"],
			["g s", "Sprints"],
			["g d", "Home"],
			["c", "New record, on a list"],
			["j / k", "Move down / up a list"],
			["x", "Select the row, for a bulk edit"],
			["Enter", "Open the row"],
			["Ctrl + /", "Toggle the AI panel"],
			["/help", "In the assistant: slash commands"],
			["t", "Cycle theme"],
			["Esc", "Close dialog or panel"],
			["?", "This list"]
		];

		/* Each key gets its own <kbd>, so "Ctrl + K" renders as two
		 * keycaps with a separator rather than one wide slab. */
		var body = rows.map(function (row) {
			var keys = row[0].split(" + ").map(function (key) {
				return "<kbd>" + key + "</kbd>";
			}).join(" + ");

			return "<tr><td>" + keys + "</td><td>" + row[1] + "</td></tr>";
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
	/*
	 * Copy-to-clipboard buttons.
	 *
	 * Delegated from the document rather than wired per element, because
	 * the run card and every other fragment re-renders itself on a poll
	 * and per-element listeners would be lost on the first swap.
	 *
	 * The value lives in data-copy rather than in the button's text, so
	 * the label can read "Copy" while the payload is a whole git command.
	 * Whatever is copied is also rendered beside the button as selectable
	 * text: the clipboard API needs a secure context and permission, and
	 * over plain http on a LAN address it simply is not there. Falling
	 * back to something a person can select by hand beats a button that
	 * silently does nothing.
	 */
	function wireCopyButtons() {
		document.addEventListener("click", function (event) {
			var button = event.target.closest("[data-copy]");
			var value;

			if (!button) {
				return;
			}

			event.preventDefault();
			value = button.getAttribute("data-copy");

			if (!value) {
				return;
			}

			if (navigator.clipboard && navigator.clipboard.writeText) {
				navigator.clipboard.writeText(value).then(function () {
					toast("Copied", "ok");
				}, function () {
					toast("Could not copy; select it instead", "warn");
				});

				return;
			}

			toast("Select the text to copy it", "warn");
		});
	}

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
			wirePickers(event.detail && event.detail.target);
			wireRecordPickers(event.detail && event.detail.target);
			wireComposer();
			wireAssist(document);
			clearTyping();

			/* The server may have OOB-swapped the hidden thread field;
			 * whatever it says now is the conversation to resume. */
			syncThreadFromInput();

			/* The runtime raises the event on the element that asked --
			 * the composer, the list button -- and names what it swapped
			 * in detail.target. The log is what matters here. */
			var swapped = (event.detail && event.detail.target) || event.target;

			if (swapped.closest && swapped.closest(".chat-log")) {
				var streaming = swapped.querySelector("[data-chat-stream]");

				syncStarters();
				wireReplyTools(swapped);
				wireChatStream(swapped);
				scrollChatToBottom();

				/*
				 * A placeholder is not an answer: the composer stays
				 * held and the launcher stays quiet until the stream
				 * says the turn is over.
				 */
				if (!streaming) {
					setChatBusy(false);

					if (swapped.querySelector(".msg.ai")) {
						notifyIfPanelClosed();
					}
				}
			}
		});

		document.body.addEventListener("htmx:sseMessage", function () {
			scrollChatToBottom();
		});

		document.body.addEventListener("htmx:responseError", function (event) {
			var status = event.detail && event.detail.status;

			clearTyping();
			setChatBusy(false);

			/* A 422 carries field-level validation messages that the
			 * server has already rendered into the form, so it should not
			 * also raise a toast. Anything else is worth surfacing. */
			if (status && status !== 422) {
				toast("Request failed (" + status + ")", "negative");
			}
		});

		document.body.addEventListener("htmx:sendError", function () {
			clearTyping();
			setChatBusy(false);
			toast("Cannot reach the server", "negative", 6000);
		});
	}

	/* ------------------------------------------------------------------ */
	/* The command palette                                                 */
	/* ------------------------------------------------------------------ */

	/*
	 * One box that goes anywhere: pages, record types with their New,
	 * and records by name. What it offers comes from /api/v1/palette,
	 * which reads the same navigation table the sidebar does, so the
	 * two cannot disagree about what exists. The last few picks are
	 * kept per browser and offered first on an empty box.
	 */
	var paletteState = null;

	function recentPicks() {
		try {
			return JSON.parse(window.localStorage.getItem(STORAGE_RECENT)
			                  || "[]");
		} catch (e) {
			return [];
		}
	}

	function rememberPick(item) {
		try {
			var list = recentPicks().filter(function (entry) {
				return entry.url !== item.url;
			});

			list.unshift({ label: item.label, url: item.url, kind: item.kind });
			window.localStorage.setItem(STORAGE_RECENT,
			                            JSON.stringify(list.slice(0, 8)));
		} catch (e) {
			/* Nothing to remember with. */
		}
	}

	function closePalette() {
		if (paletteState) {
			paletteState.backdrop.remove();
			paletteState = null;
		}
	}

	function openPalette() {
		if (paletteState) {
			paletteState.input.focus();
			return;
		}

		var backdrop = document.createElement("div");

		backdrop.className = "palette-backdrop";
		backdrop.innerHTML =
			"<div class=\"palette\" role=\"dialog\" aria-modal=\"true\">" +
			"<input class=\"palette-input\" type=\"text\" " +
			"placeholder=\"Go to, open, or make\u2026\" " +
			"autocomplete=\"off\" spellcheck=\"false\">" +
			"<ul class=\"palette-list\"></ul>" +
			"<div class=\"palette-hint\"><span><kbd>\u2191</kbd><kbd>\u2193</kbd> " +
			"move</span><span><kbd>Enter</kbd> open</span>" +
			"<span><kbd>Esc</kbd> close</span></div></div>";

		backdrop.addEventListener("click", function (event) {
			if (event.target === backdrop) {
				closePalette();
			}
		});

		document.body.appendChild(backdrop);

		paletteState = {
			backdrop: backdrop,
			input: backdrop.querySelector(".palette-input"),
			list: backdrop.querySelector(".palette-list"),
			items: [],
			cursor: 0,
			timer: null,
			request: 0
		};

		paletteState.input.addEventListener("input", function () {
			if (paletteState.timer) {
				window.clearTimeout(paletteState.timer);
			}

			paletteState.timer = window.setTimeout(refreshPalette, 120);
		});

		paletteState.input.addEventListener("keydown", function (event) {
			if (event.key === "ArrowDown") {
				event.preventDefault();
				setPaletteCursor(paletteState.cursor + 1);
			} else if (event.key === "ArrowUp") {
				event.preventDefault();
				setPaletteCursor(paletteState.cursor - 1);
			} else if (event.key === "Enter") {
				event.preventDefault();
				pickPalette(paletteState.items[paletteState.cursor]);
			} else if (event.key === "Escape") {
				event.preventDefault();
				closePalette();
			}
		});

		paletteState.input.focus();
		refreshPalette();
	}

	function setPaletteCursor(index) {
		if (!paletteState || !paletteState.items.length) {
			return;
		}

		var n = paletteState.items.length;

		paletteState.cursor = ((index % n) + n) % n;

		paletteState.list.querySelectorAll(".palette-item").forEach(function (el, i) {
			el.classList.toggle("active", i === paletteState.cursor);

			if (i === paletteState.cursor && el.scrollIntoView) {
				el.scrollIntoView({ block: "nearest" });
			}
		});
	}

	function pickPalette(item) {
		if (!item) {
			return;
		}

		rememberPick(item);
		closePalette();
		window.location.href = item.url;
	}

	function refreshPalette() {
		var q = paletteState.input.value.trim();
		var request = ++paletteState.request;

		/* An empty box offers what was picked recently, without a round
		 * trip; anything typed asks the server. */
		if (!q) {
			renderPalette([{ group: "Recent", items: recentPicks().map(function (r) {
				return { label: r.label, url: r.url, kind: r.kind || "" };
			}) }], "Type to find a page, a record type, or a record.");
			return;
		}

		fetch("/api/v1/palette?q=" + encodeURIComponent(q), {
			credentials: "same-origin",
			headers: { "Accept": "application/json" }
		}).then(function (response) {
			return response.ok ? response.json() : null;
		}).then(function (data) {
			if (!data || !paletteState || request !== paletteState.request) {
				return;
			}

			var groups = [];
			var pages = (data.pages || []).map(function (p) {
				return { label: p.label, url: p.url, kind: "page" };
			});
			var news = (data.types || []).map(function (t) {
				return { label: "New " + t.label.replace(/s$/, ""),
				         url: t.new_url, kind: t.type };
			});
			var lists = (data.types || []).map(function (t) {
				return { label: "All " + t.label, url: t.url, kind: t.type };
			});
			var records = (data.records || []).map(function (r) {
				return { label: r.label, url: r.url, kind: r.type };
			});

			if (records.length) { groups.push({ group: "Records", items: records }); }
			if (pages.length)   { groups.push({ group: "Pages", items: pages }); }
			if (lists.length)   { groups.push({ group: "Lists", items: lists }); }
			if (news.length)    { groups.push({ group: "Make", items: news }); }

			renderPalette(groups, "Nothing matches \u201c" + q + "\u201d.");
		}).catch(function () {
			/* The palette is a convenience; a failed lookup shows nothing. */
		});
	}

	function renderPalette(groups, emptyText) {
		var list = paletteState.list;
		var items = [];

		list.innerHTML = "";

		groups.forEach(function (group) {
			if (!group.items.length) {
				return;
			}

			var heading = document.createElement("li");

			heading.className = "palette-group";
			heading.textContent = group.group;
			list.appendChild(heading);

			group.items.forEach(function (item) {
				var el = document.createElement("li");
				var kind = document.createElement("span");

				el.className = "palette-item";
				el.textContent = item.label;
				kind.className = "palette-kind";
				kind.textContent = item.kind || "";
				el.appendChild(kind);

				el.addEventListener("mouseenter", function () {
					setPaletteCursor(items.indexOf(item));
				});
				el.addEventListener("click", function () {
					pickPalette(item);
				});

				list.appendChild(el);
				items.push(item);
			});
		});

		if (!items.length) {
			var empty = document.createElement("li");

			empty.className = "palette-empty";
			empty.textContent = emptyText;
			list.appendChild(empty);
		}

		paletteState.items = items;
		paletteState.cursor = 0;
		setPaletteCursor(0);
	}

	/* ------------------------------------------------------------------ */
	/* Lists: the keyboard cursor and bulk edits                          */
	/* ------------------------------------------------------------------ */

	/*
	 * j and k walk the rows of a list, Enter opens the one under the
	 * cursor, x ticks it. The cursor is a class on the row, so it is
	 * visible and survives a re-render of the bulk bar.
	 */
	function moveListCursor(key) {
		var table = document.querySelector("table[data-list]");

		if (!table) {
			return false;
		}

		var rows = Array.prototype.slice.call(table.querySelectorAll("tbody tr"));

		if (!rows.length) {
			return false;
		}

		var current = rows.findIndex(function (row) {
			return row.classList.contains("cursor");
		});

		if (key === "j" || key === "k") {
			var next = current < 0 ? 0 : current + (key === "j" ? 1 : -1);

			next = Math.max(0, Math.min(rows.length - 1, next));
			rows.forEach(function (row, i) {
				row.classList.toggle("cursor", i === next);
			});
			rows[next].scrollIntoView({ block: "nearest" });
			return true;
		}

		if (current < 0) {
			return false;
		}

		if (key === "Enter") {
			var href = rows[current].getAttribute("data-href");

			if (href) {
				window.location.href = href;
			}

			return true;
		}

		if (key === "x") {
			var box = rows[current].querySelector("[data-bulk-id]");

			if (box) {
				box.checked = !box.checked;
				box.dispatchEvent(new Event("change", { bubbles: true }));
			}

			return true;
		}

		return false;
	}

	/*
	 * The bulk bar appears when a row is ticked and carries the ids in
	 * a hidden field, so the form works exactly as it would with the
	 * ids typed in. Choosing an enum or boolean field swaps the value
	 * box for a select of its choices, which the option carries.
	 */
	function wireBulk() {
		var bar = document.querySelector("[data-bulk-bar]");

		if (!bar) {
			return;
		}

		var ids = bar.querySelector("[data-bulk-ids]");
		var count = bar.querySelector("[data-bulk-count]");
		var field = bar.querySelector("[data-bulk-field]");
		var slot = bar.querySelector("[data-bulk-value-slot]");
		var all = document.querySelector("[data-bulk-all]");

		function boxes() {
			return Array.prototype.slice.call(
				document.querySelectorAll("[data-bulk-id]"));
		}

		function sync() {
			var picked = boxes().filter(function (box) { return box.checked; });

			picked.forEach(function (box) {
				box.closest("tr").classList.add("selected");
			});
			boxes().filter(function (box) { return !box.checked; })
				.forEach(function (box) {
					box.closest("tr").classList.remove("selected");
				});

			ids.value = picked.map(function (box) {
				return box.getAttribute("data-bulk-id");
			}).join(",");
			count.textContent = picked.length + " selected";
			bar.hidden = picked.length === 0;

			if (all) {
				all.checked = picked.length > 0 && picked.length === boxes().length;
			}
		}

		function valueControl() {
			var option = field.options[field.selectedIndex];
			var choices = option ? option.getAttribute("data-options") : null;

			if (choices) {
				var select = document.createElement("select");

				select.name = "value";
				select.setAttribute("data-bulk-value", "");
				choices.split(",").forEach(function (choice) {
					var opt = document.createElement("option");

					opt.value = choice;
					opt.textContent = choice;
					select.appendChild(opt);
				});
				slot.innerHTML = "";
				slot.appendChild(select);
			} else if (!slot.querySelector("input")) {
				slot.innerHTML = "<input type=\"text\" name=\"value\" " +
				                 "placeholder=\"New value\" data-bulk-value>";
			}
		}

		document.addEventListener("change", function (event) {
			if (event.target.matches("[data-bulk-id]")) {
				sync();
			}
		});

		if (all) {
			all.addEventListener("change", function () {
				boxes().forEach(function (box) { box.checked = all.checked; });
				sync();
			});
		}

		field.addEventListener("change", valueControl);
		valueControl();

		bar.querySelector("[data-bulk-clear]").addEventListener("click", function () {
			boxes().forEach(function (box) { box.checked = false; });
			sync();
		});

		bar.querySelector("[data-bulk-delete]").addEventListener("click", function (event) {
			var n = ids.value ? ids.value.split(",").length : 0;

			if (!window.confirm("Delete " + n + " record" + (n === 1 ? "" : "s")
			                    + "? They can be restored.")) {
				event.preventDefault();
			}
		});

		sync();
	}

	/* ------------------------------------------------------------------ */
	/* The inbox and watching                                              */
	/* ------------------------------------------------------------------ */

	/*
	 * The count in the sidebar is polled gently, and marking one read is
	 * done in place: the row slides away and the count drops, without a
	 * reload. Both fall back to the plain form and page load.
	 */
	function wireInbox() {
		var badge = document.querySelector("[data-inbox-count]");

		function setCount(n) {
			if (!badge) {
				return;
			}

			var was = parseInt(badge.textContent, 10) || 0;

			badge.textContent = n;
			badge.classList.toggle("empty", n === 0);

			if (n > was) {
				badge.classList.remove("bump");
				void badge.offsetWidth;
				badge.classList.add("bump");
			}
		}

		if (badge) {
			window.setInterval(function () {
				if (document.hidden) {
					return;
				}

				fetch("/inbox/count", { credentials: "same-origin",
				                        headers: { "Accept": "application/json" } })
					.then(function (r) { return r.ok ? r.json() : null; })
					.then(function (data) {
						if (data && typeof data.unread === "number") {
							setCount(data.unread);
						}
					})
					.catch(function () {});
			}, 45000);
		}

		document.querySelectorAll("[data-inbox-read]").forEach(function (form) {
			form.addEventListener("submit", function (event) {
				var item = form.closest("[data-notification]");
				var data = new URLSearchParams(new FormData(form));

				event.preventDefault();
				data.set("async", "1");

				fetch(form.getAttribute("action"), {
					method: "POST",
					credentials: "same-origin",
					headers: { "Content-Type": "application/x-www-form-urlencoded" },
					body: data.toString()
				}).then(function (response) {
					if (!response.ok) {
						throw new Error("read failed");
					}

					if (item) {
						item.classList.add("leaving");
						window.setTimeout(function () {
							item.classList.remove("unread");
							item.classList.remove("leaving");
							form.remove();
						}, 350);
					}

					if (badge) {
						setCount(Math.max(0, (parseInt(badge.textContent, 10) || 0) - 1));
					}
				}).catch(function () {
					form.submit();
				});
			});
		});

		document.querySelectorAll("[data-watch]").forEach(function (form) {
			form.addEventListener("submit", function (event) {
				var data = new URLSearchParams(new FormData(form));
				var button = form.querySelector("button");
				var action = form.querySelector("[name=action]");

				event.preventDefault();
				data.set("async", "1");

				fetch(form.getAttribute("action"), {
					method: "POST",
					credentials: "same-origin",
					headers: { "Content-Type": "application/x-www-form-urlencoded" },
					body: data.toString()
				}).then(function (response) {
					if (!response.ok) {
						throw new Error("watch failed");
					}

					var watching = action.value === "watch";

					action.value = watching ? "unwatch" : "watch";
					button.classList.toggle("watching", watching);
					button.lastChild.textContent = watching ? " Watching" : " Watch";
					toast(watching ? "You will be told when this changes"
					               : "No longer watching", "positive");
				}).catch(function () {
					form.submit();
				});
			});
		});
	}

	/* ------------------------------------------------------------------ */
	/* The assistant's cards                                               */
	/* ------------------------------------------------------------------ */

	/*
	 * A summary or a drafted reply arrives as a card with two buttons:
	 * copy it, or put it in the comment composer. "Use it" fills the box
	 * and focuses it rather than posting: a reply that sent itself would
	 * be the one thing in this program that reached a customer without
	 * anybody reading it.
	 */
	function wireAssist(root) {
		(root || document).querySelectorAll("[data-assist-copy]").forEach(function (button) {
			if (button.ventureWired) {
				return;
			}

			button.ventureWired = true;
			button.addEventListener("click", function () {
				var body = button.closest(".assist-card")
					.querySelector("[data-assist-body]");

				if (!body || !navigator.clipboard) {
					return;
				}

				navigator.clipboard.writeText(body.textContent).then(function () {
					toast("Copied", "positive");
				}).catch(function () {
					toast("Could not copy that", "negative");
				});
			});
		});

		(root || document).querySelectorAll("[data-assist-use]").forEach(function (button) {
			if (button.ventureWired) {
				return;
			}

			button.ventureWired = true;
			button.addEventListener("click", function () {
				var body = button.closest(".assist-card")
					.querySelector("[data-assist-body]");
				var box = document.querySelector(
					".comment-composer textarea[name=body]");

				if (!body || !box) {
					return;
				}

				box.value = body.textContent;
				box.focus();
				box.scrollIntoView({ block: "center", behavior: "smooth" });
				toast("In the composer \u2014 read it before you send it",
				      "positive", 6000);
			});
		});
	}

	/*
	 * A pasted secret is shown once and is long; clicking it copies the
	 * whole thing, because selecting sixty-four characters by hand is
	 * how a character gets left behind.
	 */
	function wireSecrets(root) {
		(root || document).querySelectorAll("[data-copy]").forEach(function (block) {
			if (block.ventureWired) {
				return;
			}

			block.ventureWired = true;
			block.addEventListener("click", function () {
				if (!navigator.clipboard) {
					return;
				}

				navigator.clipboard.writeText(block.textContent.trim())
					.then(function () { toast("Copied", "positive"); })
					.catch(function () {});
			});
		});
	}

	/* ------------------------------------------------------------------ */
	/* Bootstrap                                                           */
	/* ------------------------------------------------------------------ */

	/* ------------------------------------------------------------------ */
	/* The dashboard widget editor                                         */
	/* ------------------------------------------------------------------ */

	/*
	 * Each kind option carries the settings its kind reads, so the form
	 * shows only those and the help for the chosen kind. Without this the
	 * whole form shows and still works; this is the difference between a
	 * form with five boxes and one with fifteen.
	 */
	function wireWidgetEditor() {
		var select = document.querySelector("select[data-widget-kind]");
		var help = document.querySelector("[data-widget-kind-help]");
		var always = ["title", "span", "position", "refresh_seconds"];

		if (!select) {
			return;
		}

		function apply() {
			var option = select.options[select.selectedIndex];
			var uses = option ? (option.getAttribute("data-uses") || "").split(" ") : [];

			document.querySelectorAll("[data-widget-field]").forEach(function (field) {
				var name = field.getAttribute("data-widget-field");

				if (name === "kind") {
					return;
				}

				field.hidden = always.indexOf(name) < 0 && uses.indexOf(name) < 0;
			});

			if (help && option) {
				help.textContent = option.textContent + ": reads "
					+ (uses.filter(Boolean).join(", ") || "nothing");
			}
		}

		select.addEventListener("change", apply);
		apply();
	}

	/* ------------------------------------------------------------------ */
	/* The dashboard grid editor                                           */
	/* ------------------------------------------------------------------ */

	/*
	 * Drag a card onto a cell; the server decides whether it fits and
	 * says so. Occupancy is computed here only to colour the cells while
	 * dragging -- green where the card's top-left could land, red where
	 * it would overlap or fall off the edge -- so the answer is never a
	 * surprise. The nudge buttons in each card need none of this.
	 */
	/*
	 * The dashboard grid, as a canvas.
	 *
	 * Cards are dragged with the pointer rather than dropped onto a
	 * target, and resized by their corner rather than by eight buttons.
	 * The first version of this used HTML5 drag-and-drop onto cells:
	 * correct, and nothing like moving something. A drag has to follow
	 * the cursor, show where the card will land before you let go, and
	 * leave the page where it was afterwards.
	 *
	 * The rules stay the server's. Every gesture ends in the same POST a
	 * nudge button sends, and the nudge buttons are still there for the
	 * keyboard and for a browser with no scripting -- this is a nicer
	 * way to reach the same endpoint, not a second implementation of
	 * placement.
	 */
	function wireGridEditor() {
		var grid = document.querySelector("[data-grid-editor]");

		if (!grid) {
			return;
		}

		var columns = parseInt(grid.dataset.gridColumns, 10) || 1;
		var slug = window.location.pathname.split("/")[2] || "";
		var MAX_ROWS = 60;
		var drag = null;
		var ghost = null;

		function cards() {
			return Array.prototype.slice.call(
				grid.querySelectorAll("[data-widget]"));
		}

		function box(card) {
			return {
				col: parseInt(card.dataset.col, 10) || 1,
				row: parseInt(card.dataset.row, 10) || 1,
				width: parseInt(card.dataset.width, 10) || 1,
				height: parseInt(card.dataset.height, 10) || 1
			};
		}

		/*
		 * Where every column and row actually is, measured from the
		 * cells the browser has already laid out.
		 *
		 * Not arithmetic from one cell's size, which is what this did
		 * first and what made a drag land in row 15 of a nine-row page:
		 * the rows are minmax(150px, auto), so a row holding a list is
		 * half as tall again as one holding a count, and multiplying a
		 * pointer offset by "the" row height is multiplying by a number
		 * that does not exist. The cells are laid out by the same grid
		 * as the cards, so their boxes are the answer rather than an
		 * estimate of it.
		 */
		function geometry() {
			var cells = Array.prototype.slice.call(
				grid.querySelectorAll("[data-cell]"));
			var cols = [];
			var rows = [];

			cells.forEach(function (cell) {
				var col = parseInt(cell.dataset.col, 10);
				var row = parseInt(cell.dataset.row, 10);
				var rect = cell.getBoundingClientRect();

				if (rect.width < 1 || rect.height < 1) {
					return;
				}

				if (row === 1) {
					cols[col] = { start: rect.left, end: rect.right };
				}

				if (col === 1) {
					rows[row] = { start: rect.top, end: rect.bottom };
				}
			});

			/*
			 * No measurable cell means the grid is not a grid right
			 * now: under 900px every card collapses to one column and
			 * the cells are hidden, so there is nowhere meaningful to
			 * drop and no drag to start.
			 */
			if (!cols.length || !rows.length) {
				return null;
			}

			return { cols: cols, rows: rows };
		}

		/*
		 * Which column or row a coordinate falls in. Past either end it
		 * clamps, so dragging off the edge pins to the edge rather than
		 * doing nothing.
		 */
		function slotAt(track, position) {
			var first = 0;
			var last = 0;
			var i;

			for (i = 1; i < track.length; i++) {
				if (!track[i]) {
					continue;
				}

				if (!first) {
					first = i;
				}

				last = i;

				if (position < track[i].end) {
					return Math.max(i, first);
				}
			}

			return last || 1;
		}

		function clamp(value, low, high) {
			return Math.max(low, Math.min(high, value));
		}

		/* Which card, if any, sits under a rectangle. */
		function occupants(area, except) {
			return cards().filter(function (card) {
				var other = box(card);

				if (card === except) {
					return false;
				}

				return area.col < other.col + other.width
					&& other.col < area.col + area.width
					&& area.row < other.row + other.height
					&& other.row < area.row + area.height;
			});
		}

		function fits(area) {
			return area.col >= 1 && area.row >= 1 && area.width >= 1
				&& area.height >= 1
				&& area.col + area.width - 1 <= columns
				&& area.row + area.height - 1 <= MAX_ROWS;
		}

		/*
		 * The landing rectangle. Green where the card will go, amber
		 * where it will trade places with the card already there, red
		 * where it cannot go at all.
		 */
		function showGhost(area, state) {
			if (!ghost) {
				ghost = document.createElement("div");
				ghost.className = "grid-ghost";
				grid.appendChild(ghost);
			}

			ghost.style.gridColumn = area.col + " / span " + area.width;
			ghost.style.gridRow = area.row + " / span " + area.height;
			ghost.className = "grid-ghost " + state;
		}

		function hideGhost() {
			if (ghost) {
				ghost.remove();
				ghost = null;
			}
		}

		function setCard(card, area) {
			card.style.gridColumn = area.col + " / span " + area.width;
			card.style.gridRow = area.row + " / span " + area.height;
			card.dataset.col = String(area.col);
			card.dataset.row = String(area.row);
			card.dataset.width = String(area.width);
			card.dataset.height = String(area.height);

			var where = card.querySelector(".widget-where");

			if (where) {
				where.textContent = area.col + "," + area.row + " · "
					+ area.width + "×" + area.height;
			}
		}

		/* Every gesture lands here: one POST, the server's answer wins. */
		function send(card, path, data, area, before) {
			data.set("async", "1");

			fetch("/dashboards/" + encodeURIComponent(slug) + "/widgets/"
				+ card.dataset.widget + "/" + path, {
				method: "POST",
				headers: { "Content-Type": "application/x-www-form-urlencoded" },
				body: data.toString(),
				credentials: "same-origin"
			}).then(function (response) {
				if (response.ok) {
					return null;
				}

				return response.json().then(function (body) {
					throw new Error((body && body.error && body.error.message)
						|| "That move was refused");
				});
			}).then(function () {
				card.classList.add("settled");
				window.setTimeout(function () {
					card.classList.remove("settled");
				}, 400);
			}).catch(function (problem) {
				/* Put it back where it was and say why. The card the
				 * pointer moved is wrong now, and leaving it wrong is
				 * worse than a jump. */
				setCard(card, before);

				if (area && area.partner) {
					setCard(area.partner.card, area.partner.before);
				}

				toast(problem.message, "negative", 6000);
			});
		}

		function begin(event, card, mode) {
			var g = geometry();
			var rect;

			/* Nothing to measure: the grid has collapsed to one column,
			 * where a position on it means nothing. */
			if (!g) {
				return;
			}

			rect = card.getBoundingClientRect();

			drag = {
				card: card,
				mode: mode,
				geometry: g,
				before: box(card),
				startX: event.clientX,
				startY: event.clientY,
				originLeft: rect.left,
				originTop: rect.top,
				area: box(card),
				state: "free",
				moved: false
			};

			card.classList.add(mode === "resize" ? "resizing" : "lifting");
			grid.classList.add("busy");

			/*
			 * Capture keeps the drag alive when the pointer leaves the
			 * card, which it does immediately. It throws for a pointer
			 * the browser does not consider active; the drag still
			 * works without it as long as the pointer stays over the
			 * grid, so a failure here is not worth abandoning for.
			 */
			try {
				card.setPointerCapture(event.pointerId);
			} catch (e) {
				/* Carry on uncaptured. */
			}

			event.preventDefault();
		}

		function update(event) {
			if (!drag) {
				return;
			}

			var g = drag.geometry;
			var area;

			if (Math.abs(event.clientX - drag.startX) > 2
			    || Math.abs(event.clientY - drag.startY) > 2) {
				drag.moved = true;
			}

			if (drag.mode === "move") {
				/*
				 * The card follows the pointer with a transform, which
				 * neither reflows the grid nor fights the placement
				 * already in its style. Where it will land is worked
				 * out from its own top-left corner, not the cursor, so
				 * grabbing a card by its middle does not shift it.
				 */
				var dx = event.clientX - drag.startX;
				var dy = event.clientY - drag.startY;

				drag.card.style.transform = "translate(" + dx + "px," + dy + "px)";

				/*
				 * Measured from the card's own top-left corner plus a
				 * few pixels, not from the cursor: grabbing a card by
				 * its middle and having it jump so the cursor is its
				 * corner is the thing that makes a drag feel wrong.
				 */
				area = {
					col: clamp(slotAt(g.cols, drag.originLeft + dx + 4), 1,
						columns - drag.before.width + 1),
					row: clamp(slotAt(g.rows, drag.originTop + dy + 4), 1,
						MAX_ROWS - drag.before.height + 1),
					width: drag.before.width,
					height: drag.before.height
				};
			} else {
				/*
				 * Resizing changes the card's own span as you drag. That
				 * is safe here and nowhere else: every card on an
				 * editable grid carries an explicit placement, so
				 * nothing reflows around the one that is growing.
				 */
				/* The corner is dragged to the cell the card should end
				 * at, so the size is that cell minus where it starts. */
				area = {
					col: drag.before.col,
					row: drag.before.row,
					width: clamp(slotAt(g.cols, event.clientX)
						- drag.before.col + 1, 1,
						columns - drag.before.col + 1),
					height: clamp(slotAt(g.rows, event.clientY)
						- drag.before.row + 1, 1, 8)
				};

				setCard(drag.card, area);
			}

			var taken = fits(area) ? occupants(area, drag.card) : [];

			if (!fits(area)) {
				drag.state = "refused";
			} else if (taken.length === 0) {
				drag.state = "free";
			} else if (drag.mode === "move" && taken.length === 1
			           && box(taken[0]).width === area.width
			           && box(taken[0]).height === area.height) {
				drag.state = "swap";
				drag.partner = taken[0];
			} else {
				drag.state = "refused";
			}

			if (drag.state !== "swap") {
				drag.partner = null;
			}

			drag.area = area;
			showGhost(area, drag.state);
		}

		function finish(cancelled) {
			if (!drag) {
				return;
			}

			var card = drag.card;
			var mode = drag.mode;
			var area = drag.area;
			var before = drag.before;
			var state = drag.state;
			var partner = drag.partner;
			var moved = drag.moved;

			card.classList.remove("lifting", "resizing");
			grid.classList.remove("busy");
			card.style.transform = "";
			hideGhost();
			drag = null;

			if (cancelled || !moved) {
				setCard(card, before);
				return;
			}

			if (state === "refused") {
				setCard(card, before);
				toast(mode === "resize"
					? "It will not fit at that size."
					: "Something else is there, and they are not the same "
					  + "size to trade.", "negative");
				return;
			}

			if (state === "swap" && partner) {
				var partnerBefore = box(partner);
				var swap = new URLSearchParams();

				setCard(card, partnerBefore);
				setCard(partner, before);
				swap.set("with", partner.dataset.widget);
				send(card, "swap", swap, {
					partner: { card: partner, before: partnerBefore }
				}, before);
				return;
			}

			var data = new URLSearchParams();

			setCard(card, area);
			data.set("col", String(area.col));
			data.set("row", String(area.row));
			data.set("width", String(area.width));
			data.set("height", String(area.height));
			send(card, "place", data, null, before);
		}

		grid.addEventListener("pointerdown", function (event) {
			var card = event.target.closest("[data-widget]");

			if (!card || event.button !== 0 || drag) {
				return;
			}

			if (event.target.closest("[data-resize]")) {
				begin(event, card, "resize");
				return;
			}

			/* Anything you could click is not a handle. */
			if (event.target.closest("a, button, input, select, textarea, "
			                         + "label")) {
				return;
			}

			begin(event, card, "move");
		});

		grid.addEventListener("pointermove", update);

		grid.addEventListener("pointerup", function () {
			finish(false);
		});

		grid.addEventListener("pointercancel", function () {
			finish(true);
		});

		document.addEventListener("keydown", function (event) {
			if (drag && event.key === "Escape") {
				event.preventDefault();
				finish(true);
			}
		});
	}

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

		document.querySelectorAll("[data-ai-export]").forEach(function (el) {
			el.addEventListener("click", exportThread);
		});

		document.querySelectorAll("[data-ai-starter]").forEach(function (el) {
			el.addEventListener("click", function () {
				askQuestion(el.textContent);
			});
		});

		wireChatLogClicks();
		syncStarters();

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
		wireAttachments();
		wirePodEditor();
		wireReplyTools(document);
		wireChatStream(document);
		wirePickers(document);
		wireRecordPickers(document);

		document.addEventListener("mousedown", function (event) {
			if (openPicker && !openPicker.wrap.contains(event.target)) {
				closePicker();
			}
		});

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

	/* ------------------------------------------------------------------ */
	/* Scroll entry                                                        */
	/* ------------------------------------------------------------------ */

	/*
	 * Content blocks settle into place as they scroll into view.
	 *
	 * The hidden state is opted into here rather than in the stylesheet:
	 * .reveal-ready goes on <html> only once this runs, so a browser with
	 * scripting off, or one where the script fails to parse, renders every
	 * block visible instead of a permanently blank page. That failure mode
	 * is the reason this pattern is usually a bad idea, and it is cheap to
	 * avoid.
	 *
	 * IntersectionObserver rather than a scroll listener: a scroll handler
	 * fires on every frame of every scroll for the life of the page, and
	 * this needs to know one thing once per element.
	 */
	function wireReveal(root) {
		var targets;
		var observer;
		var reduced;

		if (!window.IntersectionObserver) {
			return;
		}

		/* Honoured here as well as in the stylesheet: with motion off
		 * there is nothing to observe, so do not pay for the observer. */
		reduced = window.matchMedia
			&& window.matchMedia("(prefers-reduced-motion: reduce)").matches;

		if (reduced) {
			return;
		}

		targets = root.querySelectorAll(REVEAL_SELECTOR);

		if (!targets.length) {
			return;
		}

		document.documentElement.classList.add("reveal-ready");

		observer = new IntersectionObserver(function (entries) {
			entries.forEach(function (entry) {
				if (!entry.isIntersecting) {
					return;
				}

				entry.target.classList.add("revealed");

				/* Once shown, stop watching it and drop the compositor
				 * hint the transition needed. */
				observer.unobserve(entry.target);
				window.setTimeout(function () {
					entry.target.classList.add("reveal-done");
				}, 900);
			});
		}, { rootMargin: "0px 0px -8% 0px", threshold: 0.01 });

		Array.prototype.forEach.call(targets, function (el, index) {
			/*
			 * The cascade is capped and restarts per group. Uncapped, the
			 * twentieth card on a dense page waits 1.6s before appearing,
			 * which reads as the page being broken rather than as motion.
			 */
			el.setAttribute("data-reveal", "");
			el.style.setProperty("--reveal-index", String(index % 6));
			observer.observe(el);
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
		wireCopyButtons();
		wireRowLinks(document);
		wireBoard(document);
		wireServerEvents();
		wireReveal(document);
		wireWidgetEditor();
		wireGridEditor();
		wireBulk();
		wireInbox();
		wireAssist(document);
		wireSecrets(document);
		scrollChatToBottom();
	}

	window.venture = {
		toast: toast,
		openModal: openModal,
		openPalette: openPalette,
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
