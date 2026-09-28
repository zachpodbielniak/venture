/*
 * forms.js - Loads VENTURE forms into any page
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Usage:
 *   <div data-venture-form="https://crm.example/pub/form/TOKEN/fragment"></div>
 *   <script src="https://crm.example/pub/forms.js" defer></script>
 *
 * Fetches each form's markup into its element -- in the page's own DOM,
 * with no shadow root, so the page's stylesheet applies -- and submits it
 * without leaving the page, showing the server's messages beside the
 * questions. The server decides everything; this only displays it. With
 * this script blocked the form in the fragment still posts normally.
 *
 * Requests are "simple" in the CORS sense (a GET, and a form-encoded POST
 * with only an Accept header), because the server answers no preflight.
 */
(function () {
	"use strict";

	/*
	 * The same summary the server renders without script: a count, then
	 * one link per refused answer to its question, in the order asked.
	 * Focus moves to it, so a screen reader hears what went wrong first
	 * and a keyboard can walk to each place; the messages beside the
	 * questions are tied to them with aria-describedby already.
	 */
	function showErrors(form, errors, message) {
		var summary = form.querySelector(".vf-errors");
		var keys = Object.keys(errors || {});
		var list = document.createElement("ul");

		form.querySelectorAll(".vf-error").forEach(function (box) {
			box.textContent = "";
			box.hidden = true;
		});
		form.querySelectorAll("[aria-invalid]").forEach(function (input) {
			input.removeAttribute("aria-invalid");
		});
		list.className = "vf-error-list";

		form.querySelectorAll("[data-vf-field]").forEach(function (field) {
			var key = field.getAttribute("data-vf-field");
			var box = field.querySelector(".vf-error");
			var target = field.querySelector(".vf-input");
			var label = field.querySelector(".vf-label");
			var item, link;

			if (!Object.prototype.hasOwnProperty.call(errors || {}, key)) {
				return;
			}
			if (box) {
				box.textContent = errors[key];
				box.hidden = false;
			}
			field.querySelectorAll(".vf-input").forEach(function (input) {
				input.setAttribute("aria-invalid", "true");
			});
			if (field.getAttribute("role")) {
				field.setAttribute("aria-invalid", "true");
			}
			item = document.createElement("li");
			link = document.createElement("a");
			link.href = "#" + (target ? target.id : "");
			link.textContent = (label ? label.textContent.replace(/\*\s*$/, "").trim() : key) +
				": " + errors[key];
			item.appendChild(link);
			list.appendChild(item);
			keys.splice(keys.indexOf(key), 1);
		});

		/* Names that are no question here have nowhere to link to. */
		keys.forEach(function (key) {
			var item = document.createElement("li");

			item.textContent = (key === "_form" ? "" : key + ": ") + errors[key];
			list.appendChild(item);
		});

		if (!summary) {
			return;
		}
		summary.textContent = "";
		if (message) {
			var title = document.createElement("p");

			title.className = "vf-errors-title";
			title.textContent = message;
			summary.appendChild(title);
		}
		if (list.children.length) {
			summary.appendChild(list);
		}
		summary.hidden = !message && !list.children.length;
		if (!summary.hidden) {
			summary.setAttribute("tabindex", "-1");
			summary.focus();
		}
	}

	function showSuccess(form, message) {
		var done = document.createElement("div");

		done.className = "vf-success";
		done.setAttribute("role", "status");
		done.textContent = message;
		form.parentNode.replaceChild(done, form);
		done.setAttribute("tabindex", "-1");
		done.focus();
	}

	function wire(form) {
		if (form.ventureFormWired) {
			return;
		}
		form.ventureFormWired = true;

		form.addEventListener("submit", function (event) {
			var button = form.querySelector(".vf-submit");
			var body = new URLSearchParams(new FormData(form));

			event.preventDefault();
			if (form.ventureSending) {
				return;
			}
			form.ventureSending = true;
			if (button) {
				button.disabled = true;
			}

			fetch(form.action, {
				method: "POST",
				mode: "cors",
				credentials: "omit",
				headers: { "Accept": "application/json" },
				body: body
			}).then(function (response) {
				return response.json().catch(function () {
					return { ok: false, message: "The form could not be sent. Please try again." };
				});
			}).then(function (result) {
				if (result.ok && result.redirect) {
					window.location.assign(result.redirect);
					return;
				}
				if (result.ok) {
					showSuccess(form, result.message);
					return;
				}
				showErrors(form, result.errors, result.message);
			}).catch(function () {
				showErrors(form, null, "The form could not be sent. Check your connection and try again.");
			}).then(function () {
				form.ventureSending = false;
				if (button) {
					button.disabled = false;
				}
			});
		});
	}

	function load(target) {
		var source = target.getAttribute("data-venture-form");

		if (!source || target.ventureFormLoaded) {
			return;
		}
		target.ventureFormLoaded = true;

		fetch(source, { mode: "cors", credentials: "omit" }).then(function (response) {
			if (!response.ok) {
				throw new Error("unavailable");
			}
			return response.text();
		}).then(function (html) {
			target.innerHTML = html;
			target.querySelectorAll("form.vf-form").forEach(wire);
		}).catch(function () {
			target.textContent = "This form is not available.";
		});
	}

	function start() {
		document.querySelectorAll("[data-venture-form]").forEach(load);
	}

	if (document.readyState === "loading") {
		document.addEventListener("DOMContentLoaded", start);
	} else {
		start();
	}
}());
