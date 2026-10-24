/* 由 embedgen 自动生成，勿手改 */
.section .rodata
.global np_asset_app_js
.global np_asset_app_js_end
np_asset_app_js:
.incbin "web/app.js"
np_asset_app_js_end:
.global np_asset_assets_default_bg_webp
.global np_asset_assets_default_bg_webp_end
np_asset_assets_default_bg_webp:
.incbin "web/assets/default-bg.webp"
np_asset_assets_default_bg_webp_end:
.global np_asset_assets_default_bg1_webp
.global np_asset_assets_default_bg1_webp_end
np_asset_assets_default_bg1_webp:
.incbin "web/assets/default-bg1.webp"
np_asset_assets_default_bg1_webp_end:
.global np_asset_assets_default_bg2_webp
.global np_asset_assets_default_bg2_webp_end
np_asset_assets_default_bg2_webp:
.incbin "web/assets/default-bg2.webp"
np_asset_assets_default_bg2_webp_end:
.global np_asset_assets_default_bg3_webp
.global np_asset_assets_default_bg3_webp_end
np_asset_assets_default_bg3_webp:
.incbin "web/assets/default-bg3.webp"
np_asset_assets_default_bg3_webp_end:
.global np_asset_index_html
.global np_asset_index_html_end
np_asset_index_html:
.incbin "web/index.html"
np_asset_index_html_end:
.global np_asset_style_css
.global np_asset_style_css_end
np_asset_style_css:
.incbin "web/style.css"
np_asset_style_css_end:
.global np_asset_vendor_sortable_min_js
.global np_asset_vendor_sortable_min_js_end
np_asset_vendor_sortable_min_js:
.incbin "web/vendor/sortable.min.js"
np_asset_vendor_sortable_min_js_end:
#ifdef __ELF__
.section .note.GNU-stack,"",@progbits
#endif
