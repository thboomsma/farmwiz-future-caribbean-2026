# FarmWiz — Future Caribbean Project Overview

## Problem and Caribbean relevance

Many farmers still depend on manual checks of water, crops, environmental conditions, irrigation, and equipment. Sensor data can reduce blind spots, but raw readings still require interpretation. FarmWiz is being developed in Suriname for practical farm conditions, local budgets, mixed infrastructure, and connectivity constraints that are relevant across the Caribbean.

## Solution

FarmWiz combines IoT sensing, real-time monitoring, local automation, and AI-assisted farm intelligence. The pre-existing FarmWiz foundation provides sensing, ThingsBoard monitoring, and Switchbox scheduling/control. Future Caribbean work adds an isolated AI layer that retrieves read-only telemetry and history, checks deterministic findings, adds crop and weather context, and returns grounded recommendations.

## What was built during Future Caribbean

The Buildathon contribution is the Phase 2 FastAPI application: dashboard, read-only telemetry connector, bounded history, farm-health findings, weather and geocoded location context, sourced crop profiles, AI provider adapters, image and speech interaction, and structured approval-gated mock actions. The platform is not presented as fully autonomous physical control; the public action path ends at a mock edge.

## Users and value

Farmers and farm operators can inspect current conditions, investigate trends, understand unusual readings, compare conditions with crop and weather context, and decide what to do next. The system aims to make existing farm data more useful without removing farmer judgment.

## Business model

FarmWiz plans a hybrid hardware-plus-software model: affordable sensor nodes and farm systems, with software tiers for monitoring, history, alerts, crop and weather context, AI-assisted insights, automation workflows, and support. This is a planned model, not a claim of established revenue, customers, or partnerships.

## Go-to-market

The intended path is local-first and evidence-led: validate the system with real farm workflows, demonstrate measurable value in monitoring and decision support, learn from farmers and operators, and expand through appropriate local channels and partnerships. Higher-risk physical control follows only after field validation and safety review.

