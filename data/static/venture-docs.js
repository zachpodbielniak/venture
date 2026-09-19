/*
 * venture-docs.js - Client-side search for the documentation site
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Embedded into venturectl and written beside the pages by `docs build`.
 * No build step, no library, no service: the generator writes
 * search-index.json beside the pages, this fetches it the first time
 * the box is focused, and every keystroke filters it in the browser.
 * A page is a hit when every word typed appears in its title or text.
 */

(function () {
	"use strict";

	var box = document.getElementById("docs-search");
	var list = document.getElementById("docs-results");

	if (!box || !list) {
		return;
	}

	var index = null;
	var loading = null;

	function load() {
		if (index) {
			return Promise.resolve(index);
		}

		if (!loading) {
			loading = fetch("search-index.json", { credentials: "same-origin" })
				.then(function (response) {
					if (!response.ok) {
						throw new Error("search index: HTTP " + response.status);
					}
					return response.json();
				})
				.then(function (entries) {
					index = entries.map(function (entry) {
						return {
							url: entry.url,
							title: entry.title,
							description: entry.description || "",
							text: entry.text || "",
							haystack: (entry.title + " " + (entry.description || "") + " " + (entry.text || "")).toLowerCase()
						};
					});
					return index;
				});
		}

		return loading;
	}

	function escapeHtml(text) {
		return text.replace(/[&<>"']/g, function (c) {
			return { "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;" }[c];
		});
	}

	/* A little context around the first word, with the words marked. */
	function snippet(entry, words) {
		var lower = entry.text.toLowerCase();
		var at = -1;
		var i;

		for (i = 0; i < words.length && at < 0; i++) {
			at = lower.indexOf(words[i]);
		}

		if (at < 0) {
			return escapeHtml(entry.description || entry.text.slice(0, 120));
		}

		var start = Math.max(0, at - 50);
		var end = Math.min(entry.text.length, at + 90);
		var piece = (start > 0 ? "…" : "") + entry.text.slice(start, end) + (end < entry.text.length ? "…" : "");
		var html = escapeHtml(piece);

		words.forEach(function (word) {
			if (!word) {
				return;
			}
			var pattern = new RegExp(word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "ig");
			html = html.replace(pattern, function (m) { return "<mark>" + m + "</mark>"; });
		});

		return html;
	}

	function score(entry, words) {
		var total = 0;
		var i;

		for (i = 0; i < words.length; i++) {
			if (entry.haystack.indexOf(words[i]) < 0) {
				return -1;
			}
			if (entry.title.toLowerCase().indexOf(words[i]) >= 0) {
				total += 10;
			}
			total += 1;
		}

		return total;
	}

	function render(results, words) {
		list.innerHTML = "";

		if (!results.length) {
			var empty = document.createElement("li");
			empty.className = "docs-result-empty";
			empty.textContent = "Nothing matches.";
			list.appendChild(empty);
			list.hidden = false;
			return;
		}

		results.slice(0, 12).forEach(function (entry) {
			var item = document.createElement("li");
			var link = document.createElement("a");
			link.href = entry.url;
			link.innerHTML = "<span class=\"docs-result-title\">" + escapeHtml(entry.title) + "</span>" +
				"<span class=\"docs-result-snippet\">" + snippet(entry, words) + "</span>";
			item.appendChild(link);
			list.appendChild(item);
		});

		list.hidden = false;
	}

	function search() {
		var query = box.value.trim().toLowerCase();

		if (!query) {
			list.hidden = true;
			list.innerHTML = "";
			return;
		}

		var words = query.split(/\s+/).filter(Boolean);

		load().then(function (entries) {
			var results = entries
				.map(function (entry) { return { entry: entry, score: score(entry, words) }; })
				.filter(function (hit) { return hit.score >= 0; })
				.sort(function (a, b) { return b.score - a.score || a.entry.title.localeCompare(b.entry.title); })
				.map(function (hit) { return hit.entry; });
			render(results, words);
		}).catch(function (error) {
			list.innerHTML = "";
			var item = document.createElement("li");
			item.className = "docs-result-empty";
			item.textContent = "Search is unavailable: " + error.message;
			list.appendChild(item);
			list.hidden = false;
		});
	}

	var timer = null;

	box.addEventListener("focus", function () { load().catch(function () {}); });
	box.addEventListener("input", function () {
		clearTimeout(timer);
		timer = setTimeout(search, 80);
	});
	box.addEventListener("keydown", function (event) {
		if (event.key === "Escape") {
			box.value = "";
			list.hidden = true;
			list.innerHTML = "";
		} else if (event.key === "Enter") {
			var first = list.querySelector("a");
			if (first) {
				window.location.href = first.getAttribute("href");
			}
		} else if (event.key === "ArrowDown") {
			var link = list.querySelector("a");
			if (link) {
				event.preventDefault();
				link.focus();
			}
		}
	});
	list.addEventListener("keydown", function (event) {
		var links = Array.prototype.slice.call(list.querySelectorAll("a"));
		var at = links.indexOf(document.activeElement);

		if (event.key === "ArrowDown" && at >= 0 && at + 1 < links.length) {
			event.preventDefault();
			links[at + 1].focus();
		} else if (event.key === "ArrowUp" && at > 0) {
			event.preventDefault();
			links[at - 1].focus();
		} else if (event.key === "ArrowUp" && at === 0) {
			event.preventDefault();
			box.focus();
		} else if (event.key === "Escape") {
			list.hidden = true;
			box.focus();
		}
	});
	document.addEventListener("click", function (event) {
		if (!list.contains(event.target) && event.target !== box) {
			list.hidden = true;
		}
	});

	/* "/" focuses the search from anywhere on the page, as on the app. */
	document.addEventListener("keydown", function (event) {
		if (event.key === "/" && document.activeElement !== box &&
		    !/^(INPUT|TEXTAREA)$/.test(document.activeElement.tagName)) {
			event.preventDefault();
			box.focus();
		}
	});
})();
