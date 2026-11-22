# 截图工具栏图标

- `icons/` 是 [Lucide 0.468.0](https://github.com/lucide-icons/lucide/tree/f12b0de177fbc2a6795e99be065887e72b237123/icons) 的 12 个原始 SVG，均来自提交 `f12b0de177fbc2a6795e99be065887e72b237123`。未改动这些文件。
- 工具栏与弹层共用形状轮廓；椭圆从 `circle` 压缩纵向坐标、保持原线宽，色点也复用 `circle`，以当前颜色填充。两个下拉菜单共用 `chevron-down`。
- `LICENSE` 是同一提交的 Lucide ISC 全文，包含 Feather 归属；`LICENSE.feather` 是 [Feather v4.29.2](https://github.com/feathericons/feather/blob/1b002399e8758fb2bccea4d07312dc9e0f43dcb8/LICENSE) 的完整 MIT 文本。`NOTICE` 合并两份原文，由 `src/shiguang.rc` 作为 `RCDATA` 资源嵌入 EXE，独立分发 EXE 也保留归属。不随包附带 SVG、图标库、字体或渲染器。
- `SHA256SUMS` 记录原始素材和许可的 SHA-256。改动素材后从仓库根目录运行 `python3 scripts/generate_toolbar_glyphs.py`，提交更新后的 `src/toolbar_glyph_data.h`；普通 Windows 构建不需要 Python。生成器仅支持这批固定素材用到的 SVG 语法。
