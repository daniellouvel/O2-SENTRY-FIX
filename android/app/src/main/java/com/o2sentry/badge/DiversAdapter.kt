package com.o2sentry.badge

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.o2sentry.badge.databinding.ItemDiverBinding

class DiversAdapter(
    private val divers: MutableList<String>,
    private val onWrite: (String) -> Unit,
    private val onDelete: (String) -> Unit
) : RecyclerView.Adapter<DiversAdapter.ViewHolder>() {

    inner class ViewHolder(val binding: ItemDiverBinding) :
        RecyclerView.ViewHolder(binding.root)

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        ViewHolder(ItemDiverBinding.inflate(LayoutInflater.from(parent.context), parent, false))

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val name = divers[position]
        holder.binding.nameText.text = name
        holder.binding.root.setOnClickListener { onWrite(name) }
        holder.binding.deleteButton.setOnClickListener { onDelete(name) }
    }

    override fun getItemCount() = divers.size
}
