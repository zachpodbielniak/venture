/*
 * venture-hx.js - A compact hx-* attribute client for VENTURE
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * htmx-glib generates standard hx-* attributes and HX-* response headers.
 * This file implements the subset of the htmx runtime those attributes and
 * headers actually use, so the server binary is genuinely self-contained:
 * no CDN, no vendored third-party bundle, nothing to fetch at build time.
 *
 * Supported attributes:
 *   hx-get hx-post hx-put hx-patch hx-delete
 *   hx-target      this | closest S | next S | previous S | find S | S
 *   hx-swap        innerHTML outerHTML beforebegin afterbegin beforeend
 *                  afterend delete none, plus swap:Ns / settle:Ns / scroll /
 *                  show modifiers
 *   hx-trigger     event names, load, revealed, every Ns, and the
 *                  changed / once / delay:Ns / throttle:Ns / from:S modifiers
 *   hx-include hx-vals hx-params hx-headers hx-confirm hx-indicator
 *   hx-push-url hx-replace-url hx-boost hx-disabled-elt hx-sync
 *   hx-swap-oob    on returned fragments
 *   sse-connect sse-swap  for the streaming AI chat
 *
 * Supported response headers:
 *   HX-Redirect HX-Location HX-Refresh HX-Push-Url HX-Replace-Url
 *   HX-Retarget HX-Reswap HX-Reselect
 *   HX-Trigger HX-Trigger-After-Swap HX-Trigger-After-Settle
 *
 * Events are dispatched with the htmx: prefix so application code written
 * against htmx keeps working: htmx:beforeRequest, htmx:beforeSwap,
 * htmx:afterSwap, htmx:afterSettle, htmx:responseError, htmx:sendError.
 *
 * If you would rather run upstream htmx, drop htmx.min.js into
 * data/static/, add it to ASSET_FILES in rules.mk and remove this file from
 * the list. The attributes the server emits are compatible with both.
 */

(function (window, document) {
	"use strict";

	var VERBS = ["get", "post", "put", "patch", "delete"];
	var SWAP_STYLES = ["innerHTML", "outerHTML", "beforebegin", "afterbegin",
	                   "beforeend", "afterend", "delete", "none"];

	/* Requests in flight, keyed by element, so hx-sync can abort or queue. */
	var inFlight = new WeakMap();

	/* ------------------------------------------------------------------ */
	/* Small helpers                                                       */
	/* ------------------------------------------------------------------ */

	function attr(el, name) {
		return el && el.getAttribute ? el.getAttribute(name) : null;
	}

	/*
	 * Looks up an attribute on the element or, failing that, on the nearest
	 * ancestor that has it. htmx inherits most attributes down the tree,
	 * which is what lets a single hx-target on a container apply to every
	 * button inside it.
	 */
	function inheritedAttr(el, name) {
		var node = el;

		while (node && node.getAttribute) {
			var value = node.getAttribute(name);

			if (value !== null) {
				return value;
			}

			node = node.parentElement;
		}

		return null;
	}

	function trigger(el, name, detail) {
		var event = new CustomEvent(name, {
			bubbles: true,
			cancelable: true,
			detail: detail || {}
		});

		return el.dispatchEvent(event);
	}

	function splitTop(text, separator) {
		/*
		 * Splits on a separator that is not inside brackets, braces or
		 * quotes. hx-trigger values like "click from:#a, keyup[key=='x']"
		 * cannot be split with a plain String.split.
		 */
		var parts = [];
		var depth = 0;
		var quote = null;
		var current = "";
		var i;

		for (i = 0; i < text.length; i++) {
			var c = text.charAt(i);

			if (quote) {
				if (c === quote && text.charAt(i - 1) !== "\\") {
					quote = null;
				}
				current += c;
				continue;
			}

			if (c === "'" || c === '"') {
				quote = c;
				current += c;
				continue;
			}

			if (c === "[" || c === "{" || c === "(") {
				depth++;
			} else if (c === "]" || c === "}" || c === ")") {
				depth--;
			}

			if (c === separator && depth === 0) {
				parts.push(current.trim());
				current = "";
				continue;
			}

			current += c;
		}

		if (current.trim() !== "") {
			parts.push(current.trim());
		}

		return parts;
	}

	function parseSeconds(text) {
		/* Accepts "500ms", "2s", "2" (seconds). Returns milliseconds. */
		var match = /^([0-9.]+)(ms|s|m)?$/.exec(String(text).trim());

		if (!match) {
			return 0;
		}

		var value = parseFloat(match[1]);
		var unit = match[2] || "s";

		if (unit === "ms") {
			return value;
		}

		if (unit === "m") {
			return value * 60000;
		}

		return value * 1000;
	}

	/* ------------------------------------------------------------------ */
	/* Target resolution                                                   */
	/* ------------------------------------------------------------------ */

	function resolveTarget(el, spec) {
		if (!spec || spec === "this") {
			return el;
		}

		var match;

		match = /^closest\s+(.+)$/.exec(spec);
		if (match) {
			return el.closest(match[1]);
		}

		match = /^find\s+(.+)$/.exec(spec);
		if (match) {
			return el.querySelector(match[1]);
		}

		match = /^next\s+(.+)$/.exec(spec);
		if (match) {
			var next = el.nextElementSibling;

			while (next && !next.matches(match[1])) {
				next = next.nextElementSibling;
			}

			return next;
		}

		match = /^previous\s+(.+)$/.exec(spec);
		if (match) {
			var prev = el.previousElementSibling;

			while (prev && !prev.matches(match[1])) {
				prev = prev.previousElementSibling;
			}

			return prev;
		}

		if (spec === "body") {
			return document.body;
		}

		return document.querySelector(spec);
	}

	/* ------------------------------------------------------------------ */
	/* Swapping                                                            */
	/* ------------------------------------------------------------------ */

	function parseSwapSpec(spec) {
		var result = {
			style: "innerHTML",
			swapDelay: 0,
			settleDelay: 20,
			scroll: null,
			show: null,
			focusScroll: false
		};

		if (!spec) {
			return result;
		}

		var tokens = spec.trim().split(/\s+/);
		var i;

		for (i = 0; i < tokens.length; i++) {
			var token = tokens[i];

			if (SWAP_STYLES.indexOf(token) !== -1) {
				result.style = token;
				continue;
			}

			var colon = token.indexOf(":");

			if (colon === -1) {
				continue;
			}

			var key = token.slice(0, colon);
			var value = token.slice(colon + 1);

			if (key === "swap") {
				result.swapDelay = parseSeconds(value);
			} else if (key === "settle") {
				result.settleDelay = parseSeconds(value);
			} else if (key === "scroll") {
				result.scroll = value;
			} else if (key === "show") {
				result.show = value;
			} else if (key === "focus-scroll") {
				result.focusScroll = (value === "true");
			}
		}

		return result;
	}

	function parseFragment(html) {
		var template = document.createElement("template");

		template.innerHTML = html;

		return template.content;
	}

	/*
	 * Out-of-band swaps: any top-level node in the response carrying
	 * hx-swap-oob is applied to the element with the same id, wherever it
	 * is on the page, and removed from the main fragment. This is how a
	 * single response updates a table row and the header totals at once.
	 */
	function applyOobSwaps(fragment) {
		var candidates = Array.prototype.slice.call(
			fragment.querySelectorAll("[hx-swap-oob]"));
		var i;

		for (i = 0; i < candidates.length; i++) {
			var node = candidates[i];
			var oob = node.getAttribute("hx-swap-oob");
			var style = "outerHTML";
			var selector = "#" + node.id;

			node.removeAttribute("hx-swap-oob");

			if (oob && oob !== "true") {
				var colon = oob.indexOf(":");

				if (colon === -1) {
					style = oob;
				} else {
					style = oob.slice(0, colon);
					selector = oob.slice(colon + 1);
				}
			}

			var target = document.querySelector(selector);

			if (!target) {
				continue;
			}

			node.parentNode.removeChild(node);
			performSwap(target, node.outerHTML, { style: style,
			                                      settleDelay: 20 });
		}
	}

	function performSwap(target, html, swapSpec) {
		var fragment;
		var inserted = [];
		var i;

		if (!target) {
			return inserted;
		}

		if (swapSpec.style === "none") {
			return inserted;
		}

		if (swapSpec.style === "delete") {
			target.parentNode.removeChild(target);
			return inserted;
		}

		fragment = parseFragment(html);
		applyOobSwaps(fragment);

		/* Remember the nodes so they can be processed and animated after
		 * insertion; a DocumentFragment is emptied by the insert. */
		inserted = Array.prototype.slice.call(fragment.children);

		switch (swapSpec.style) {
		case "outerHTML":
			target.replaceWith(fragment);
			break;
		case "beforebegin":
			target.parentNode.insertBefore(fragment, target);
			break;
		case "afterbegin":
			target.insertBefore(fragment, target.firstChild);
			break;
		case "beforeend":
			target.appendChild(fragment);
			break;
		case "afterend":
			target.parentNode.insertBefore(fragment, target.nextSibling);
			break;
		case "innerHTML":
		default:
			target.innerHTML = "";
			target.appendChild(fragment);
			break;
		}

		for (i = 0; i < inserted.length; i++) {
			if (inserted[i].nodeType === 1) {
				inserted[i].classList.add("swapped-in");
				process(inserted[i]);
			}
		}

		if (swapSpec.scroll === "top") {
			(target.scrollTo ? target : window).scrollTo(0, 0);
		} else if (swapSpec.scroll === "bottom" && target.scrollHeight) {
			target.scrollTop = target.scrollHeight;
		}

		if (swapSpec.show === "top" && target.scrollIntoView) {
			target.scrollIntoView({ block: "start", behavior: "smooth" });
		}

		return inserted;
	}

	/* ------------------------------------------------------------------ */
	/* Request parameter collection                                        */
	/* ------------------------------------------------------------------ */

	function collectFormValues(form, params) {
		var data = new FormData(form);

		data.forEach(function (value, key) {
			params.append(key, value);
		});
	}

	function collectValues(el, verb) {
		var params = new URLSearchParams();
		var include = inheritedAttr(el, "hx-include");
		var vals = inheritedAttr(el, "hx-vals");
		var only = inheritedAttr(el, "hx-params");

		/* A form submits its own fields; a control inside a form that is
		 * not a submit still contributes only itself, matching htmx. */
		if (el.tagName === "FORM") {
			collectFormValues(el, params);
		} else if (el.name && el.value !== undefined && verb !== "get") {
			params.append(el.name, el.value);
		}

		if (include) {
			var nodes = document.querySelectorAll(include);
			var i;

			for (i = 0; i < nodes.length; i++) {
				var node = nodes[i];

				if (node.tagName === "FORM") {
					collectFormValues(node, params);
				} else if (node.name) {
					if (node.type === "checkbox" || node.type === "radio") {
						if (node.checked) {
							params.append(node.name, node.value);
						}
					} else {
						params.append(node.name, node.value);
					}
				}
			}
		}

		if (vals) {
			try {
				var parsed = JSON.parse(vals);
				var key;

				for (key in parsed) {
					if (Object.prototype.hasOwnProperty.call(parsed, key)) {
						params.set(key, parsed[key]);
					}
				}
			} catch (e) {
				console.warn("venture-hx: hx-vals is not valid JSON:", vals);
			}
		}

		/* hx-params filters what is actually sent. "none" is the useful
		 * case: a delete button inside a form should not post the form. */
		if (only) {
			if (only === "none") {
				return new URLSearchParams();
			}

			if (only !== "*") {
				var filtered = new URLSearchParams();
				var wanted;
				var negate = false;
				var list = only;

				if (list.indexOf("not ") === 0) {
					negate = true;
					list = list.slice(4);
				}

				wanted = list.split(",").map(function (s) {
					return s.trim();
				});

				params.forEach(function (value, key) {
					var present = wanted.indexOf(key) !== -1;

					if (present !== negate) {
						filtered.append(key, value);
					}
				});

				return filtered;
			}
		}

		return params;
	}

	/* ------------------------------------------------------------------ */
	/* Indicators and disabled elements                                    */
	/* ------------------------------------------------------------------ */

	function indicatorElements(el) {
		var spec = inheritedAttr(el, "hx-indicator");

		if (!spec) {
			return [el];
		}

		return Array.prototype.slice.call(document.querySelectorAll(spec));
	}

	function setRequesting(el, active) {
		var indicators = indicatorElements(el);
		var disableSpec = inheritedAttr(el, "hx-disabled-elt");
		var i;

		for (i = 0; i < indicators.length; i++) {
			indicators[i].classList.toggle("hx-request", active);
		}

		if (disableSpec) {
			var disabled = disableSpec === "this"
				? [el]
				: Array.prototype.slice.call(
					document.querySelectorAll(disableSpec));

			for (i = 0; i < disabled.length; i++) {
				disabled[i].disabled = active;
			}
		}
	}

	/* ------------------------------------------------------------------ */
	/* Response header handling                                            */
	/* ------------------------------------------------------------------ */

	function fireHeaderTriggers(headerValue) {
		if (!headerValue) {
			return;
		}

		/*
		 * HX-Trigger is either a bare event name, a comma-separated list
		 * of them, or a JSON object mapping event names to detail values.
		 */
		var text = headerValue.trim();

		if (text.charAt(0) === "{") {
			var parsed;

			try {
				parsed = JSON.parse(text);
			} catch (e) {
				console.warn("venture-hx: HX-Trigger is not valid JSON:", text);
				return;
			}

			var key;

			for (key in parsed) {
				if (Object.prototype.hasOwnProperty.call(parsed, key)) {
					trigger(document.body, key, parsed[key]);
				}
			}

			return;
		}

		text.split(",").forEach(function (name) {
			var trimmed = name.trim();

			if (trimmed) {
				trigger(document.body, trimmed, {});
			}
		});
	}

	/* ------------------------------------------------------------------ */
	/* The request itself                                                  */
	/* ------------------------------------------------------------------ */

	function issueRequest(el, verb, url, triggeringEvent) {
		var confirmMessage = inheritedAttr(el, "hx-confirm");
		var targetSpec = inheritedAttr(el, "hx-target");
		var swapSpec = parseSwapSpec(inheritedAttr(el, "hx-swap"));
		var headersSpec = inheritedAttr(el, "hx-headers");
		var syncSpec = inheritedAttr(el, "hx-sync");
		var params = collectValues(el, verb);
		var target = resolveTarget(el, targetSpec);
		var requestUrl = url;
		var body = null;
		var headers = {};
		var controller = new AbortController();
		var existing = inFlight.get(el);

		if (confirmMessage && !window.confirm(confirmMessage)) {
			return;
		}

		/* hx-sync="this:replace" (the default when hx-sync names this
		 * element) aborts a request already running for the element, which
		 * is what a search-as-you-type box wants. */
		if (existing && syncSpec !== "queue") {
			existing.abort();
		}

		if (!trigger(el, "htmx:beforeRequest", { verb: verb, url: url,
		                                         target: target })) {
			return;
		}

		headers["HX-Request"] = "true";
		headers["HX-Current-URL"] = window.location.href;

		if (el.id) {
			headers["HX-Trigger"] = el.id;
		}

		if (el.getAttribute("name")) {
			headers["HX-Trigger-Name"] = el.getAttribute("name");
		}

		if (target && target.id) {
			headers["HX-Target"] = target.id;
		}

		if (headersSpec) {
			try {
				var extra = JSON.parse(headersSpec);
				var key;

				for (key in extra) {
					if (Object.prototype.hasOwnProperty.call(extra, key)) {
						headers[key] = extra[key];
					}
				}
			} catch (e) {
				console.warn("venture-hx: hx-headers is not valid JSON:",
				             headersSpec);
			}
		}

		if (verb === "get" || verb === "delete") {
			var query = params.toString();

			if (query) {
				requestUrl += (requestUrl.indexOf("?") === -1 ? "?" : "&") + query;
			}
		} else {
			body = params;
			headers["Content-Type"] =
				"application/x-www-form-urlencoded; charset=UTF-8";
		}

		setRequesting(el, true);
		inFlight.set(el, controller);

		fetch(requestUrl, {
			method: verb.toUpperCase(),
			headers: headers,
			body: body,
			credentials: "same-origin",
			signal: controller.signal
		}).then(function (response) {
			return response.text().then(function (text) {
				return { response: response, text: text };
			});
		}).then(function (result) {
			var response = result.response;
			var text = result.text;
			var h = response.headers;
			var finalTarget = target;
			var finalSwap = swapSpec;

			inFlight.delete(el);
			setRequesting(el, false);

			if (h.get("HX-Redirect")) {
				window.location.href = h.get("HX-Redirect");
				return;
			}

			if (h.get("HX-Location")) {
				window.location.href = h.get("HX-Location");
				return;
			}

			if (h.get("HX-Refresh") === "true") {
				window.location.reload();
				return;
			}

			if (h.get("HX-Retarget")) {
				finalTarget = resolveTarget(el, h.get("HX-Retarget"));
			}

			if (h.get("HX-Reswap")) {
				finalSwap = parseSwapSpec(h.get("HX-Reswap"));
			}

			if (!response.ok) {
				/*
				 * A 4xx or 5xx still carries a rendered error fragment
				 * from the server in this application, so it is swapped
				 * in rather than discarded -- that is how validation
				 * messages reach the form. Listeners can cancel this.
				 */
				if (!trigger(el, "htmx:responseError", {
					xhr: response, status: response.status, body: text
				})) {
					return;
				}
			}

			if (h.get("HX-Reselect")) {
				var selected = parseFragment(text)
					.querySelector(h.get("HX-Reselect"));

				text = selected ? selected.outerHTML : "";
			}

			if (!trigger(el, "htmx:beforeSwap", { target: finalTarget,
			                                      body: text })) {
				return;
			}

			var doSwap = function () {
				performSwap(finalTarget, text, finalSwap);
				trigger(el, "htmx:afterSwap", { target: finalTarget });
				fireHeaderTriggers(h.get("HX-Trigger"));
				fireHeaderTriggers(h.get("HX-Trigger-After-Swap"));

				window.setTimeout(function () {
					trigger(el, "htmx:afterSettle", { target: finalTarget });
					fireHeaderTriggers(h.get("HX-Trigger-After-Settle"));
				}, finalSwap.settleDelay);
			};

			if (finalSwap.swapDelay > 0) {
				window.setTimeout(doSwap, finalSwap.swapDelay);
			} else {
				doSwap();
			}

			var pushUrl = h.get("HX-Push-Url") || inheritedAttr(el, "hx-push-url");
			var replaceUrl = h.get("HX-Replace-Url")
				|| inheritedAttr(el, "hx-replace-url");

			if (pushUrl && pushUrl !== "false") {
				window.history.pushState({ ventureHx: true }, "",
					pushUrl === "true" ? requestUrl : pushUrl);
			} else if (replaceUrl && replaceUrl !== "false") {
				window.history.replaceState({ ventureHx: true }, "",
					replaceUrl === "true" ? requestUrl : replaceUrl);
			}
		}).catch(function (error) {
			inFlight.delete(el);
			setRequesting(el, false);

			if (error.name === "AbortError") {
				return;
			}

			console.error("venture-hx: request failed:", error);
			trigger(el, "htmx:sendError", { error: error });
		});
	}

	/* ------------------------------------------------------------------ */
	/* Trigger binding                                                     */
	/* ------------------------------------------------------------------ */

	function defaultTriggerFor(el) {
		var tag = el.tagName;

		if (tag === "FORM") {
			return "submit";
		}

		if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") {
			return "change";
		}

		return "click";
	}

	function parseTriggerSpec(text) {
		var spec = {
			event: null,
			filter: null,
			once: false,
			changed: false,
			delay: 0,
			throttle: 0,
			from: null,
			target: null,
			every: 0,
			preventDefault: true
		};

		var tokens = text.trim().split(/\s+/);
		var head = tokens.shift();
		var bracket = head.indexOf("[");

		if (bracket !== -1 && head.charAt(head.length - 1) === "]") {
			spec.filter = head.slice(bracket + 1, head.length - 1);
			head = head.slice(0, bracket);
		}

		spec.event = head;

		var i;

		for (i = 0; i < tokens.length; i++) {
			var token = tokens[i];

			if (token === "once") {
				spec.once = true;
			} else if (token === "changed") {
				spec.changed = true;
			} else if (token.indexOf("delay:") === 0) {
				spec.delay = parseSeconds(token.slice(6));
			} else if (token.indexOf("throttle:") === 0) {
				spec.throttle = parseSeconds(token.slice(9));
			} else if (token.indexOf("from:") === 0) {
				spec.from = token.slice(5);
			} else if (token.indexOf("target:") === 0) {
				spec.target = token.slice(7);
			}
		}

		/* "every 2s" arrives as event "every" followed by the interval. */
		if (spec.event === "every") {
			spec.every = parseSeconds(tokens[0] || "1s");
		}

		return spec;
	}

	function verbAndUrl(el) {
		var i;

		for (i = 0; i < VERBS.length; i++) {
			var url = attr(el, "hx-" + VERBS[i]);

			if (url !== null) {
				return { verb: VERBS[i], url: url };
			}
		}

		return null;
	}

	function bindElement(el) {
		if (el.ventureHxBound) {
			return;
		}

		var request = verbAndUrl(el);

		if (!request) {
			return;
		}

		el.ventureHxBound = true;

		var triggerAttr = attr(el, "hx-trigger") || defaultTriggerFor(el);
		var specs = splitTop(triggerAttr, ",").map(parseTriggerSpec);

		specs.forEach(function (spec) {
			var lastValue = el.value;
			var lastFired = 0;
			var fired = false;

			var fire = function (event) {
				var now = Date.now();

				if (spec.once && fired) {
					return;
				}

				if (spec.changed) {
					if (el.value === lastValue) {
						return;
					}

					lastValue = el.value;
				}

				if (spec.throttle > 0 && (now - lastFired) < spec.throttle) {
					return;
				}

				if (spec.filter) {
					var allowed;

					try {
						/* The filter is authored by the server, not by a
						 * user, so evaluating it is no more privileged
						 * than the page itself. */
						allowed = new Function("event", "return (" +
						                       spec.filter + ");").call(el, event);
					} catch (e) {
						console.warn("venture-hx: bad trigger filter:",
						             spec.filter, e);
						allowed = false;
					}

					if (!allowed) {
						return;
					}
				}

				lastFired = now;
				fired = true;

				if (event && spec.event === "submit") {
					event.preventDefault();
				} else if (event && spec.event === "click"
				           && el.tagName === "A") {
					event.preventDefault();
				}

				var go = function () {
					issueRequest(el, request.verb, request.url, event);
				};

				if (spec.delay > 0) {
					window.clearTimeout(el.ventureHxDelay);
					el.ventureHxDelay = window.setTimeout(go, spec.delay);
				} else {
					go();
				}
			};

			if (spec.event === "load") {
				window.setTimeout(function () { fire(null); }, spec.delay);
				return;
			}

			if (spec.event === "every") {
				window.setInterval(function () { fire(null); },
				                   spec.every || 1000);
				return;
			}

			if (spec.event === "revealed") {
				if (!window.IntersectionObserver) {
					fire(null);
					return;
				}

				var observer = new IntersectionObserver(function (entries) {
					if (entries[0].isIntersecting) {
						observer.disconnect();
						fire(null);
					}
				});

				observer.observe(el);
				return;
			}

			var source = spec.from
				? (spec.from === "body" ? document.body
				                        : document.querySelector(spec.from))
				: el;

			if (source) {
				source.addEventListener(spec.event, fire);
			}
		});
	}

	/* ------------------------------------------------------------------ */
	/* hx-boost                                                            */
	/* ------------------------------------------------------------------ */

	function bindBoosted(el) {
		if (el.ventureHxBoosted) {
			return;
		}

		var boosted = inheritedAttr(el, "hx-boost");

		if (boosted !== "true") {
			return;
		}

		if (el.tagName === "A" && el.getAttribute("href")
		    && el.getAttribute("href").charAt(0) !== "#"
		    && !el.getAttribute("target")) {
			el.ventureHxBoosted = true;
			el.addEventListener("click", function (event) {
				event.preventDefault();
				el.setAttribute("hx-get", el.getAttribute("href"));
				el.setAttribute("hx-target",
					inheritedAttr(el, "hx-target") || "body");
				el.setAttribute("hx-push-url", "true");
				issueRequest(el, "get", el.getAttribute("href"), event);
			});
		} else if (el.tagName === "FORM") {
			el.ventureHxBoosted = true;
			el.addEventListener("submit", function (event) {
				event.preventDefault();

				var method = (el.getAttribute("method") || "get").toLowerCase();

				issueRequest(el, method, el.getAttribute("action") || "",
				             event);
			});
		}
	}

	/* ------------------------------------------------------------------ */
	/* Server-sent events, used by the AI chat stream                      */
	/* ------------------------------------------------------------------ */

	function bindSse(el) {
		if (el.ventureSseBound) {
			return;
		}

		var url = attr(el, "sse-connect");

		if (!url) {
			return;
		}

		el.ventureSseBound = true;

		var source = new EventSource(url, { withCredentials: true });
		var swapNames = (attr(el, "sse-swap") || "message")
			.split(",").map(function (s) { return s.trim(); });
		var swapSpec = parseSwapSpec(attr(el, "hx-swap") || "beforeend");
		var target = resolveTarget(el, attr(el, "hx-target")) || el;

		swapNames.forEach(function (name) {
			source.addEventListener(name, function (event) {
				performSwap(target, event.data, swapSpec);
				trigger(el, "htmx:sseMessage", { name: name,
				                                 data: event.data });
			});
		});

		source.addEventListener("error", function () {
			trigger(el, "htmx:sseError", {});
		});

		/* Closing on removal matters: without it every navigation leaks a
		 * live connection, and the chat dock reconnects on each page. */
		el.ventureSseClose = function () { source.close(); };
	}

	/* ------------------------------------------------------------------ */
	/* Processing and bootstrap                                            */
	/* ------------------------------------------------------------------ */

	function process(root) {
		var scope = root || document.body;
		var nodes;
		var i;

		if (scope.nodeType === 1) {
			bindElement(scope);
			bindBoosted(scope);
			bindSse(scope);
		}

		nodes = scope.querySelectorAll(
			"[hx-get],[hx-post],[hx-put],[hx-patch],[hx-delete]," +
			"[hx-boost] a,[hx-boost] form,[sse-connect]");

		for (i = 0; i < nodes.length; i++) {
			bindElement(nodes[i]);
			bindBoosted(nodes[i]);
			bindSse(nodes[i]);
		}
	}

	/*
	 * A MutationObserver keeps server-rendered fragments live without every
	 * swap having to remember to call process(); performSwap does call it,
	 * but content inserted by application code gets picked up too.
	 */
	function observe() {
		var observer = new MutationObserver(function (mutations) {
			var i;
			var j;

			for (i = 0; i < mutations.length; i++) {
				for (j = 0; j < mutations[i].addedNodes.length; j++) {
					var node = mutations[i].addedNodes[j];

					if (node.nodeType === 1) {
						process(node);
					}
				}

				for (j = 0; j < mutations[i].removedNodes.length; j++) {
					var removed = mutations[i].removedNodes[j];

					if (removed.nodeType === 1 && removed.ventureSseClose) {
						removed.ventureSseClose();
					}
				}
			}
		});

		observer.observe(document.body, { childList: true, subtree: true });
	}

	/* Public surface, deliberately small and htmx-compatible in name. */
	window.htmx = {
		process: process,
		ajax: function (verb, url, targetSpec) {
			var el = document.body;
			var proxy = document.createElement("span");

			proxy.setAttribute("hx-target", targetSpec || "body");
			el.appendChild(proxy);
			issueRequest(proxy, verb.toLowerCase(), url, null);
			el.removeChild(proxy);
		},
		trigger: trigger,
		find: function (selector) { return document.querySelector(selector); },
		version: "venture-hx 0.1"
	};

	if (document.readyState === "loading") {
		document.addEventListener("DOMContentLoaded", function () {
			process(document.body);
			observe();
		});
	} else {
		process(document.body);
		observe();
	}
}(window, document));
