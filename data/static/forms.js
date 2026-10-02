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

	/* Matches the server's closed condition vocabulary. Prior-page outcomes
	 * are supplied as booleans, never as hidden copies of earlier answers. */
	function wireRules(form) {
		var rules = JSON.parse(form.getAttribute("data-vf-rules") || "[]");
		var page = Number(form.getAttribute("data-vf-page") || 0);
		if (!rules.length) { return; }
		function atom(actual, op, wanted) {
			if (op === "equals" || op === "not_equals") { return actual === wanted; }
			if (op === "contains") { return actual.indexOf(wanted) !== -1; }
			if (op === "any_of") { return wanted.split("\n").indexOf(actual) !== -1; }
			var number = Number(actual);
			if (!actual || actual !== actual.trimEnd() || !Number.isFinite(number)) { return false; }
			return op === "greater_than" ? number > Number(wanted) : op === "less_than" && number < Number(wanted);
		}
		function match(rule, values) {
			if (rule.from_page !== page) { return rule.matched; }
			var matches = rule.conditions.map(function (condition, index) {
				if (!form.elements.namedItem(condition.field)) { return rule.condition_matches[index]; }
				var answers = values.getAll(condition.field);
				if (condition.operator === "is_empty") { return !answers.length || answers.every(function (x) { return x === ""; }); }
				if (!answers.length) { answers = [""]; }
				var found = answers.some(function (x) { return atom(x, condition.operator, condition.value); });
				return condition.operator === "not_equals" ? !found : found;
			});
			return rule.any ? matches.some(Boolean) : matches.every(Boolean);
		}
		function update() {
			form.querySelectorAll("[data-vf-field]").forEach(function (field) {
				var key = field.getAttribute("data-vf-field");
				var values = new FormData(form);
				var active = true, required = field.getAttribute("data-vf-required-base") === "true";
				rules.forEach(function (rule) {
					if (rule.target !== key) { return; }
					var matched = match(rule, values);
					if (rule.action === 0 && !matched) { active = false; }
					if (rule.action === 1 && matched) { active = false; }
					if (rule.action === 2 && matched) { required = true; }
				});
				field.hidden = !active;
				var controls = field.matches("input,textarea,select") ? [field] : field.querySelectorAll("input,textarea,select");
				controls.forEach(function (input) {
					input.disabled = !active;
					input.required = active && required && field.getAttribute("data-vf-kind") !== "multiple_choice";
					if (input.required) { input.setAttribute("aria-required", "true"); }
					else { input.removeAttribute("aria-required"); }
				});
				if (field.getAttribute("role") === "radiogroup") {
					if (active && required) { field.setAttribute("aria-required", "true"); }
					else { field.removeAttribute("aria-required"); }
				}
				var label = field.querySelector(".vf-label");
				var marker = field.querySelector(".vf-required");
				if (label && required && !marker) {
					marker = document.createElement("span"); marker.className = "vf-required";
					marker.setAttribute("aria-hidden", "true"); marker.textContent = "*"; label.appendChild(marker);
				}
				if (marker) { marker.hidden = !required; }
			});
		}
		form.addEventListener("input", update);
		form.addEventListener("change", update);
		update();
	}

	/* The server supplies only referenced public values from earlier pages.
	 * Current input stays text: no expression evaluation or HTML insertion. */
	function wirePiping(form) {
		var context = JSON.parse(form.getAttribute("data-vf-piping") || "{}");
		var previous = context.values || {}, fields = context.fields || {};
		function read(key) {
			var own = Object.prototype.hasOwnProperty;
			var info = own.call(fields, key) ? fields[key] : null, named = form.elements.namedItem(key);
			if (!named || !info) { return own.call(previous, key) ? previous[key] : ""; }
			var controls = named.tagName ? [named] : Array.from(named);
			if (!controls.length || controls.every(function (input) { return input.disabled; })) { return ""; }
			if (controls.some(function (input) { return !input.disabled && !input.checkValidity(); })) { return ""; }
			var raw = new FormData(form).getAll(key);
			if (info.kind === "checkbox") { return raw.length ? "Yes" : "No"; }
			if (info.kind === "consent") { return raw.length ? "Given" : ""; }
			if (!raw.length) { return info.kind === "hidden" ? (info.default || "") : ""; }
			var value = String(raw[0]).trim();
			if (info.kind === "hidden" && !value) { value = info.default || ""; }
			if (info.kind === "email" && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(value)) { return ""; }
			if (info.kind === "phone" && (!/^[0-9+() .-]+$/.test(value) || (value.match(/[0-9]/g) || []).length < 4)) { return ""; }
			if (info.kind === "number" || info.kind === "rating") { return value && Number.isFinite(Number(value)) ? String(Number(value)) : ""; }
			if (info.kind === "single_choice" || info.kind === "multiple_choice") {
				if (raw.some(function (item) { return !Object.prototype.hasOwnProperty.call(info.choices, item); })) { return ""; }
				return raw.map(function (item) { return info.choices[item]; }).join(", ");
			}
			return value;
		}
		function update() {
			form.querySelectorAll("[data-vf-pipe]").forEach(function (node) {
				var template = node.getAttribute("data-vf-pipe");
				var holder = node.closest("[data-vf-field]");
				var target = holder ? holder.getAttribute("data-vf-field") : "";
				var row = target.match(/^([a-z][a-z0-9_]*\[[0-9]+\])\[/);
				var value = template.replace(/\{\{|\}\}|\{([a-z][a-z0-9_]*(?:\.count)?)\}/g, function (match, key) {
					if (match === "{{") { return "{"; } if (match === "}}") { return "}"; }
					if (row && Object.prototype.hasOwnProperty.call(fields, row[1] + "[" + key + "]")) { key = row[1] + "[" + key + "]"; }
					return read(key);
				}).slice(0, 32768);
				if (node.tagName === "DIV") {
					node.replaceChildren();
					value.split("\n\n").forEach(function (block) {
						if (!block.trim()) { return; }
						var paragraph = document.createElement("p");
						block.trim().split("\n").forEach(function (line, index) {
							if (index) { paragraph.appendChild(document.createElement("br")); }
							paragraph.appendChild(document.createTextNode(line));
						});
						node.appendChild(paragraph);
					});
				} else { node.textContent = value; }
			});
		}
		form.addEventListener("input", update); form.addEventListener("change", update); update();
	}

	function wire(form) {
		if (form.ventureFormWired) {
			return;
		}
		form.ventureFormWired = true;
		wireRules(form);
		wirePiping(form);

		form.addEventListener("submit", function (event) {
			var button = form.querySelector(".vf-submit");
			var body = new URLSearchParams(new FormData(form));
			if (event.submitter && event.submitter.name) {
				body.append(event.submitter.name, event.submitter.value);
			}

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
				if (result.html) {
					var holder = document.createElement("template");
					holder.innerHTML = result.html;
					var next = holder.content.querySelector("form.vf-form");
					if (!next) { throw new Error("invalid page"); }
					form.replaceWith(next);
					wire(next);
					var focus = next.querySelector(".vf-errors:not([hidden])") ||
						next.querySelector(".vf-progress") || next.querySelector(".vf-input");
					if (focus) { focus.focus(); }
					return;
				}
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
