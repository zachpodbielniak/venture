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

						appendUserEcho(log, echo);
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
			wireComposer();
			wireAssist(document);
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
	function wireGridEditor() {
		var grid = document.querySelector("[data-grid-editor]");

		if (!grid) {
			return;
		}

		var columns = parseInt(grid.dataset.gridColumns, 10) || 1;
		var dragging = null;

		function cards() {
			return Array.prototype.slice.call(grid.querySelectorAll("[data-widget]"));
		}

		function fits(card, col, row) {
			var width = parseInt(card.dataset.width, 10) || 1;
			var height = parseInt(card.dataset.height, 10) || 1;

			if (col < 1 || row < 1 || col + width - 1 > columns) {
				return false;
			}

			return cards().every(function (other) {
				if (other === card) {
					return true;
				}

				var oc = parseInt(other.dataset.col, 10);
				var or = parseInt(other.dataset.row, 10);
				var ow = parseInt(other.dataset.width, 10) || 1;
				var oh = parseInt(other.dataset.height, 10) || 1;

				return !(col < oc + ow && oc < col + width
					&& row < or + oh && or < row + height);
			});
		}

		function clearCells() {
			Array.prototype.forEach.call(
				grid.querySelectorAll("[data-cell]"),
				function (cell) {
					cell.classList.remove("drop-target", "drop-refused");
				}
			);
		}

		grid.addEventListener("dragstart", function (event) {
			var card = event.target.closest("[data-widget]");

			if (!card) {
				return;
			}

			dragging = card;
			card.classList.add("dragging");
			event.dataTransfer.effectAllowed = "move";
			event.dataTransfer.setData("text/plain", card.dataset.widget);
		});

		grid.addEventListener("dragend", function () {
			if (dragging) {
				dragging.classList.remove("dragging");
			}

			dragging = null;
			clearCells();
		});

		grid.addEventListener("dragover", function (event) {
			var cell = event.target.closest("[data-cell]");

			if (!cell || !dragging) {
				return;
			}

			event.preventDefault();
			clearCells();

			var col = parseInt(cell.dataset.col, 10);
			var row = parseInt(cell.dataset.row, 10);

			if (fits(dragging, col, row)) {
				event.dataTransfer.dropEffect = "move";
				cell.classList.add("drop-target");
			} else {
				event.dataTransfer.dropEffect = "none";
				cell.classList.add("drop-refused");
			}
		});

		grid.addEventListener("drop", function (event) {
			var cell = event.target.closest("[data-cell]");

			if (!cell || !dragging) {
				return;
			}

			event.preventDefault();

			var card = dragging;
			var col = parseInt(cell.dataset.col, 10);
			var row = parseInt(cell.dataset.row, 10);
			var slug = window.location.pathname.split("/")[2];
			var data = new URLSearchParams();

			clearCells();

			if (!fits(card, col, row)) {
				toast("That spot is taken, or the card would not fit.",
				      "negative");
				return;
			}

			/* Move it now; the server is asked to agree, and the page
			 * reloads so the layout is the server's, not this guess. */
			card.style.gridColumn = col + " / span " + card.dataset.width;
			card.style.gridRow = row + " / span " + card.dataset.height;
			card.dataset.col = String(col);
			card.dataset.row = String(row);

			data.set("col", String(col));
			data.set("row", String(row));
			data.set("async", "1");

			fetch("/dashboards/" + encodeURIComponent(slug) + "/widgets/"
				+ card.dataset.widget + "/place", {
				method: "POST",
				headers: { "Content-Type": "application/x-www-form-urlencoded" },
				body: data.toString(),
				credentials: "same-origin"
			}).then(function (response) {
				if (!response.ok) {
					return response.json().then(function (body) {
						throw new Error((body && body.error && body.error.message)
							|| "The move was refused");
					});
				}

				window.location.reload();
			}).catch(function (problem) {
				toast(problem.message, "negative");
				window.location.reload();
			});
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
