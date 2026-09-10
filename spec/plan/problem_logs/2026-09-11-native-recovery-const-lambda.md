# Native復旧処理のconstラムダによるWindowsビルド失敗

- Date: 2026-09-11
- Area: VulkanContext::recreate_swapchain

## Summary
Pictor #1640の登録buildがexit 1で失敗。configureは成功したが、保存ログは末尾に切り詰められてエラー行が残っていなかった。

## Evidence
対象head: 9dc7dd4。MSVC 14.39の単一翻訳単位コンパイルで、vulkan_context.cppのfailed()呼び出しにC2440とC3848を再現。Dx12Contextの翻訳単位はコンパイル成功。

## Cause
`const auto failed`に格納したラムダへ`mutable`を指定していたため、非constのoperator()をconstオブジェクトから呼んでいた。ラムダが更新するものはthisが指すメンバーと参照捕捉した変数であり、値捕捉した変数の変更はない。

## Fix Requirements
不要なmutableを除き、constのまま呼べるようにする。復旧処理の所有権・解放順や失敗条件は変更しない。

## Verification
修正前にMSVCでC2440/C3848を再現し、修正後に同じVulkanContext翻訳単位を再コンパイルして確認する。登録configure/build全体の再検証はRevisorへ依頼する。単体・実機・起動テストは本修正で実施しない。

## Follow-up
Revisorの登録build成功を確認してからマージする。全体ビルドの成功と実機での復旧動作は別の判定とする。
