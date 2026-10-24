-- Additive: the workflow defaults that had no home before the settings API
-- (docs/settings.org). Every one arrives empty or off, which reads as its
-- documented default: link checks only when asked, a 9 o'clock local-time
-- hour, a 4-hour A/B wait won on opens, resends after 3 days, and the
-- built-in STOP, START and HELP answers. Nothing already stored moves.
SELECT mail_preview_link_check, marketing_local_hour, marketing_ab_wait_hours, marketing_ab_metric, marketing_resend_days, telephony_stop_reply, telephony_start_reply, telephony_help_reply FROM organizations WHERE 1 = 0;
